/*---------------------------------------------------------------------------*\
              Original work | Copyright (C) 2016-2017 DHI
              Modified work | Copyright (C) 2016-2017 OpenCFD Ltd.
              Modified work | Copyright (C) 2017-2018 Johan Roenby
-------------------------------------------------------------------------------

License
    This file is part of isoAdvector which is an extension to OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

\*---------------------------------------------------------------------------*/

#include "isoAdvection.H"
#include "volFields.H"
#include "interpolationCellPoint.H"
#include "volPointInterpolation.H"
#include "fvcSurfaceIntegrate.H"
#include "fvcGrad.H"
#include "upwind.H"
#include "cellSet.H"
#include "meshTools.H"
#include "OBJstream.H"

// * * * * * * * * * * * * * * Debugging * * * * * * * * * * * * * //

#ifndef DebugInfo
//Taken from OpenFOAM-4.0/src/OpenFOAM/db/error/messageStream.H to make code
//compile with older OF versions.
#define DebugInfo                                                              \
    if (debug) Info
#endif

#ifndef InfoInFunction
#define InfoInFunction InfoIn(__func__)
#endif

#ifndef DebugInFunction
#define DebugInFunction                                                        \
    if (debug) InfoInFunction
#endif

// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //
namespace Foam
{
    defineTypeNameAndDebug(isoAdvection, 0);
}

// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

Foam::isoAdvection::isoAdvection
(
    volScalarField& alpha1,
    const surfaceScalarField& phi,
    const volVectorField& U
)
:
    // General data
    mesh_(alpha1.mesh()),
    dict_(mesh_.solverDict(alpha1.name())),
    alpha1_(alpha1),
    alpha1In_(alpha1.ref()),
    phi_(phi),
    U_(U),
    dVf_
    (
        IOobject
        (
            "dVf_",
            mesh_.time().timeName(),
            mesh_,
            IOobject::NO_READ,
            IOobject::NO_WRITE
        ),
        mesh_,
        dimensionedScalar("zero", dimVol, 0)
    ),
    advectionTime_(0),

    // Interpolation data
    ap_(mesh_.nPoints()),

    // Tolerances and solution controls
    nAlphaBounds_(dict_.lookupOrDefault<label>("nAlphaBounds", 3)),
    isoFaceTol_(dict_.lookupOrDefault<scalar>("isoFaceTol", 1e-10)),
    surfCellTol_(dict_.lookupOrDefault<scalar>("surfCellTol", 1e-8)),
    gradAlphaBasedNormal_
    (
        dict_.lookupOrDefault<bool>("gradAlphaNormal", false)
    ),
    writeIsoFacesToFile_
    (
        dict_.lookupOrDefault<bool>("writeIsoFaces", false)
    ),

    // Cell cutting data
    surfCells_(label(0.2*mesh_.nCells())),
    isoCutCell_(mesh_, ap_),
    isoCutFace_(mesh_, ap_),
    cellIsBounded_(mesh_.nCells()),
    checkBounding_(mesh_.nCells()),
    bsFaces_(label(0.2*(mesh_.nFaces() - mesh_.nInternalFaces()))),
    bsx0_(bsFaces_.size()),
    bsn0_(bsFaces_.size()),
    bsUn0_(bsFaces_.size()),
    bsf0_(bsFaces_.size()),

    // Parallel run data
    procPatchLabels_(mesh_.boundary().size()),
    surfaceCellFacesOnProcPatches_(0)
{
    isoCutCell::debug = debug;
    isoCutFace::debug = debug;

    // Prepare lists used in parallel runs
    if (Pstream::parRun())
    {
        // Force calculation of required demand driven data (else parallel
        // communication may crash)
        mesh_.cellCentres();
        mesh_.cellVolumes();
        mesh_.faceCentres();
        mesh_.faceAreas();
        mesh_.magSf();
        mesh_.boundaryMesh().patchID();
        mesh_.cellPoints();
        mesh_.cellCells();
        mesh_.cells();

        // Get boundary mesh and resize the list for parallel comms
        const polyBoundaryMesh& patches = mesh_.boundaryMesh();

        surfaceCellFacesOnProcPatches_.resize(patches.size());

        // Append all processor patch labels to the list
        forAll(patches, patchi)
        {
            if
            (
                isA<processorPolyPatch>(patches[patchi])
             && patches[patchi].size() > 0
            )
            {
                procPatchLabels_.append(patchi);
            }
        }
    }
}


// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //

void Foam::isoAdvection::timeIntegratedFlux()
{
    // Get time step
    const scalar dt = mesh_.time().deltaTValue();

    // Create object for interpolating velocity to isoface centres
    interpolationCellPoint<vector> UInterp(U_);

    // For each downwind face of each surface cell we "isoadvect" to find dVf
    label nSurfaceCells = 0;

    // Clear out the data for re-use and reset list containing information
    // whether cells could possibly need bounding
    clearIsoFaceData();

    // Get necessary references
    const scalarField& phiIn = phi_.primitiveField();
    const scalarField& magSfIn = mesh_.magSf().primitiveField();
    scalarField& dVfIn = dVf_.primitiveFieldRef();

    // Get necessary mesh data
    const cellList& cellFaces = mesh_.cells();
    const labelList& own = mesh_.faceOwner();
    const labelList& nei = mesh_.faceNeighbour();
    const labelListList& cellCells = mesh_.cellCells();

    // Calculate alpha vertex values, ap_, or cell normals (used to get
    // interface-vertex distance function if gradAlphaBasedNormal_)
    volVectorField cellNormals("cellN", fvc::grad(alpha1_));
    if (gradAlphaBasedNormal_)
    {
        // Calculate gradient of alpha1 and normalise and smoothen it.
        normaliseAndSmooth(cellNormals);
    }
    else
    {
        // Interpolating alpha1 cell centre values to mesh points (vertices)
        ap_ = volPointInterpolation::New(mesh_).interpolate(alpha1_);
    }

    // Storage for isoFace points. Only used if writeIsoFacesToFile_
    DynamicList<List<point> > isoFacePts;

    // Loop through all cells
    forAll(alpha1In_, celli)
    {
        // If not a surface cell continue to next cell
        if (!isASurfaceCell(celli)) continue;

        // This is a surface cell, increment counter, append and mark cell
        // Note: We also have the cellStatus below where the cell might not have
        // an isoface. So maybe the counter and append should be put there.
        nSurfaceCells++;
        surfCells_.append(celli);
        checkBounding_[celli] = true;

        DebugInfo
            << "\n------------ Cell " << celli << " with alpha1 = "
            << alpha1In_[celli] << " and 1-alpha1 = "
            << 1.0 - alpha1In_[celli] << " ------------"
            << endl;

        if (gradAlphaBasedNormal_)
        {
            vectorField& cellNormalsIn = cellNormals.primitiveFieldRef();
            setCellVertexValues(celli, cellNormalsIn);
        }

        // Calculate isoFace centre x0, normal n0 at time t

        // Calculate cell status (-1: cell is fully below the isosurface, 0:
        // cell is cut, 1: cell is fully above the isosurface)
        label maxIter = 100; // NOTE: make it a debug switch
        label cellStatus = isoCutCell_.vofCutCell
        (
            celli,
            alpha1In_[celli],
            isoFaceTol_,
            maxIter
        );

        // If cell is not cut move on to next cell
        if (cellStatus != 0) continue;

        // If cell is cut calculate isoface unit normal
        const scalar f0(isoCutCell_.isoValue());
        const point& x0(isoCutCell_.isoFaceCentre());
        vector n0(isoCutCell_.isoFaceArea());
        n0 /= (mag(n0));

        if (writeIsoFacesToFile_ && mesh_.time().writeTime())
        {
            isoFacePts.append(isoCutCell_.isoFacePoints());
        }

        // Get the speed of the isoface by interpolating velocity and
        // dotting it with isoface unit normal
        const scalar Un0 = UInterp.interpolate(x0, celli) & n0;

        DebugInfo
            << "calcIsoFace gives initial surface: \nx0 = " << x0
            << ", \nn0 = " << n0 << ", \nf0 = " << f0 << ", \nUn0 = "
            << Un0 << endl;

        // Estimate time integrated flux through each downwind face
        // Note: looping over all cell faces - in reduced-D, some of
        //       these faces will be on empty patches
        const cell& celliFaces = cellFaces[celli];
        forAll(celliFaces, fi)
        {
            const label facei = celliFaces[fi];

            if (mesh_.isInternalFace(facei))
            {
                bool isDownwindFace = false;
                label otherCell = -1;

                if (celli == own[facei])
                {
                    if (phiIn[facei] > 10*SMALL)
                    {
                        isDownwindFace = true;
                    }

                    otherCell = nei[facei];
                }
                else
                {
                    if (phiIn[facei] < -10*SMALL)
                    {
                        isDownwindFace = true;
                    }

                    otherCell = own[facei];
                }

                if (isDownwindFace)
                {
                    dVfIn[facei] = isoCutFace_.timeIntegratedFaceFlux
                    (
                        facei,
                        x0,
                        n0,
                        Un0,
                        f0,
                        dt,
                        phiIn[facei],
                        magSfIn[facei]
                    );
                }

                // We want to check bounding of neighbour cells to
                // surface cells as well:
                checkBounding_[otherCell] = true;

                // Also check neighbours of neighbours.
                // Note: consider making it a run time selectable
                // extension level (easily done with recursion):
                // 0 - only neighbours
                // 1 - neighbours of neighbours
                // 2 - ...
                // Note: We will like all point neighbours to interface cells to
                // be checked. Especially if the interface leaves a cell during
                // a time step, it may enter a point neighbour which should also
                // be treated like a surface cell. Its interface normal should
                // somehow be inherrited from its upwind cells from which it
                // receives the interface.
                const labelList& nNeighbourCells = cellCells[otherCell];
                forAll(nNeighbourCells, ni)
                {
                    checkBounding_[nNeighbourCells[ni]] = true;
                }
            }
            else
            {
                bsFaces_.append(facei);
                bsx0_.append(x0);
                bsn0_.append(n0);
                bsUn0_.append(Un0);
                bsf0_.append(f0);

                // Note: we must not check if the face is on the
                // processor patch here.
            }
        }
    }

    // Get references to boundary fields
    const polyBoundaryMesh& boundaryMesh = mesh_.boundaryMesh();
    const surfaceScalarField::Boundary& phib = phi_.boundaryField();
    const surfaceScalarField::Boundary& magSfb = mesh_.magSf().boundaryField();
    surfaceScalarField::Boundary& dVfb = dVf_.boundaryFieldRef();
    const label nInternalFaces = mesh_.nInternalFaces();

    // Loop through boundary surface faces
    forAll(bsFaces_, i)
    {
        // Get boundary face index (in the global list)
        const label facei = bsFaces_[i];
        const label patchi = boundaryMesh.patchID()[facei - nInternalFaces];
        const label start = boundaryMesh[patchi].start();

        if (phib[patchi].size())
        {
            const label patchFacei = facei - start;
            const scalar phiP = phib[patchi][patchFacei];

            if (phiP > 10*SMALL)
            {
                const scalar magSf = magSfb[patchi][patchFacei];

                dVfb[patchi][patchFacei] = isoCutFace_.timeIntegratedFaceFlux
                (
                    facei,
                    bsx0_[i],
                    bsn0_[i],
                    bsUn0_[i],
                    bsf0_[i],
                    dt,
                    phiP,
                    magSf
                );

                // Check if the face is on processor patch and append it to
                // the list if necessary
                checkIfOnProcPatch(facei);
            }
        }
    }

    // Synchronize processor patches
    syncProcPatches(dVf_, phi_);

    writeIsoFaces(isoFacePts);

    Info<< "Number of isoAdvector surface cells = "
        << returnReduce(nSurfaceCells, sumOp<label>()) << endl;
}


void Foam::isoAdvection::setCellVertexValues
(
    const label celli,
    const vectorField& cellNormalsIn
)
{
    const labelListList& cellPoints = mesh_.cellPoints();
    const vectorField& cellCentres = mesh_.cellCentres();
    const pointField& points = mesh_.points();
    const labelList& cp = cellPoints[celli];
    const point& cellCentre = cellCentres[celli];
    forAll(cp, vi)
    {
        const point& vertex = points[cp[vi]];
        ap_[cp[vi]] = (vertex - cellCentre) & cellNormalsIn[celli];
    }
}


void Foam::isoAdvection::normaliseAndSmooth
(
    volVectorField& cellN
)
{
    const labelListList& cellPoints = mesh_.cellPoints();
    const vectorField& cellCentres = mesh_.cellCentres();
    const pointField& points = mesh_.points();

    vectorField& cellNIn = cellN.primitiveFieldRef();
    cellNIn /= (mag(cellNIn) + SMALL);
    vectorField vertexN(mesh_.nPoints(), vector::zero);
    vertexN = volPointInterpolation::New(mesh_).interpolate(cellN);
    vertexN /= (mag(vertexN) + SMALL);
    // Interpolate vertex normals back to cells
    forAll(cellNIn, celli)
    {
        const labelList& cp = cellPoints[celli];
        vector cellNi = vector::zero;
        const point& cellCentre = cellCentres[celli];
        forAll(cp, pointI)
        {
            point vertex = points[cp[pointI]];
            scalar w = 1.0/mag(vertex - cellCentre);
            cellNi += w*vertexN[cp[pointI]];
        }
        cellNIn[celli] = cellNi/(mag(cellNi) + SMALL);
    }
}


void Foam::isoAdvection::setDownwindFaces
(
    const label celli,
    DynamicLabelList& downwindFaces
) const
{
    DebugInFunction << endl;

    // Get necessary mesh data and cell information
    const labelList& own = mesh_.faceOwner();
    const cellList& cells = mesh_.cells();
    const cell& c = cells[celli];

    downwindFaces.clear();

    // Check all faces of the cell
    forAll(c, fi)
    {
        // Get face and corresponding flux
        const label facei = c[fi];
        const scalar phi = faceValue(phi_, facei);

        if (own[facei] == celli)
        {
            if (phi > 10*SMALL)
            {
                downwindFaces.append(facei);
            }
        }
        else if (phi < -10*SMALL)
        {
            downwindFaces.append(facei);
        }
    }

    downwindFaces.shrink();
}


void Foam::isoAdvection::limitFluxes()
{
    DebugInFunction << endl;

    // Get time step size
    const scalar dt = mesh_.time().deltaT().value();

    volScalarField alphaNew = alpha1_ - fvc::surfaceIntegrate(dVf_);
    const scalar aTol = 1.0e-12;          // Note: tolerances
    scalar maxAlphaMinus1 = gMax(alphaNew) - 1;      // max(alphaNew - 1);
    scalar minAlpha = gMin(alphaNew);           // min(alphaNew);
    const label nUndershoots = 20;        // sum(neg0(alphaNew + aTol));
    const label nOvershoots = 20;         // sum(pos0(alphaNew - 1 - aTol));
    cellIsBounded_ = false;

    Info << "isoAdvection: Before conservative bounding: min(alpha) = "
        << minAlpha << ", max(alpha) = 1 + " << maxAlphaMinus1 << endl;

    // Loop number of bounding steps
    for (label n = 0; n < nAlphaBounds_; n++)
    {
        if (maxAlphaMinus1 > aTol) // Note: tolerances
        {
            DebugInfo << "Bound from above... " << endl;

//          scalarField& dVfcorrected = dVf_.primitiveFieldRef();

            surfaceScalarField dVfcorrected("dVfcorrected", dVf_);
            DynamicList<label> correctedFaces(3*nOvershoots);
            boundFromAbove(alpha1In_, dVfcorrected, correctedFaces);

            forAll(correctedFaces, fi)
            {
                label facei = correctedFaces[fi];

                // Change to treat boundaries consistently
                setFaceValue(dVf_, facei, faceValue(dVfcorrected, facei));
            }

            syncProcPatches(dVf_, phi_);
        }

        if (minAlpha < -aTol) // Note: tolerances
        {
            DebugInfo << "Bound from below... " << endl;

            scalarField alpha2(1.0 - alpha1In_);
            surfaceScalarField dVfcorrected
            (
                "dVfcorrected",
                phi_*dimensionedScalar("dt", dimTime, dt) - dVf_
            );
//          dVfcorrected -= dVf_;   // phi_ and dVf_ have same sign and dVf_ is
                                    // the portion of phi_*dt that is water.
            // If phi_ > 0 then dVf_ > 0 and mag(phi_*dt-dVf_) < mag(phi_*dt) as
            // it should.
            // If phi_ < 0 then dVf_ < 0 and mag(phi_*dt-dVf_) < mag(phi_*dt) as
            // it should.
            DynamicList<label> correctedFaces(3*nUndershoots);
            boundFromAbove(alpha2, dVfcorrected, correctedFaces);
            forAll(correctedFaces, fi)
            {
                label facei = correctedFaces[fi];

                // Change to treat boundaries consistently
                scalar phi = faceValue(phi_, facei);
                scalar dVcorr = faceValue(dVfcorrected, facei);
                setFaceValue(dVf_, facei, phi*dt - dVcorr);
            }

            syncProcPatches(dVf_, phi_);
        }

        if (debug)
        {
            // Check if still unbounded
            scalarField alphaNew(alpha1In_ - fvc::surfaceIntegrate(dVf_)());
            label maxAlphaMinus1 = max(alphaNew - 1);
            scalar minAlpha = min(alphaNew);
            label nUndershoots = sum(neg0(alphaNew + aTol));
            label nOvershoots = sum(pos0(alphaNew - 1 - aTol));
            Info<< "After bounding number " << n + 1 << " of time "
                << mesh_.time().value() << ":" << endl;
            Info<< "nOvershoots = " << nOvershoots << " with max(alphaNew-1) = "
                << maxAlphaMinus1 << " and nUndershoots = " << nUndershoots
                << " with min(alphaNew) = " << minAlpha << endl;
        }
    }
}


void Foam::isoAdvection::boundFromAbove
(
    const scalarField& alpha1,
    surfaceScalarField& dVf,
    DynamicList<label>& correctedFaces
)
{
    DebugInFunction << endl;

    correctedFaces.clear();
    scalar aTol = 10*SMALL; // Note: tolerances

    const scalarField& meshV = mesh_.cellVolumes();
    const scalar dt = mesh_.time().deltaTValue();

    DynamicList<label> downwindFaces(10);
    DynamicList<label> facesToPassFluidThrough(downwindFaces.size());
    DynamicList<scalar> dVfmax(downwindFaces.size());
    DynamicList<scalar> phi(downwindFaces.size());

    // Loop through alpha cell centred field
    forAll(alpha1, celli)
    {
        if (checkBounding_[celli])
        {
            const scalar Vi = meshV[celli];
            scalar alpha1New = alpha1[celli] - netFlux(dVf, celli)/Vi;
            scalar alphaOvershoot = alpha1New - 1.0;
            scalar fluidToPassOn = alphaOvershoot*Vi;
            label nFacesToPassFluidThrough = 1;

            bool firstLoop = true;

            // First try to pass surplus fluid on to neighbour cells that are
            // not filled and to which dVf < phi*dt
            while (alphaOvershoot > aTol && nFacesToPassFluidThrough > 0)
            {
                DebugInfo
                    << "\n\nBounding cell " << celli
                    << " with alpha overshooting " << alphaOvershoot
                    << endl;

                facesToPassFluidThrough.clear();
                dVfmax.clear();
                phi.clear();

                cellIsBounded_[celli] = true;

                // Find potential neighbour cells to pass surplus phase to
                setDownwindFaces(celli, downwindFaces);

                scalar dVftot = 0;
                nFacesToPassFluidThrough = 0;

                forAll(downwindFaces, fi)
                {
                    const label facei = downwindFaces[fi];
                    const scalar phif = faceValue(phi_, facei);
                    const scalar dVff = faceValue(dVf, facei);
                    const scalar maxExtraFaceFluidTrans = mag(phif*dt - dVff);

                    // dVf has same sign as phi and so if phi>0 we have
                    // mag(phi_[facei]*dt) - mag(dVf[facei]) = phi_[facei]*dt
                    // - dVf[facei]
                    // If phi < 0 we have mag(phi_[facei]*dt) -
                    // mag(dVf[facei]) = -phi_[facei]*dt - (-dVf[facei]) > 0
                    // since mag(dVf) < phi*dt
                    DebugInfo
                        << "downwindFace " << facei
                        << " has maxExtraFaceFluidTrans = "
                        << maxExtraFaceFluidTrans << endl;

                    if (maxExtraFaceFluidTrans/Vi > aTol)
                    {
//                    if (maxExtraFaceFluidTrans/Vi > aTol &&
//                    mag(dVfIn[facei])/Vi > aTol) //Last condition may be
//                    important because without this we will flux through uncut
//                    downwind faces
                        facesToPassFluidThrough.append(facei);
                        phi.append(phif);
                        dVfmax.append(maxExtraFaceFluidTrans);
                        dVftot += mag(phif*dt);
                    }
                }

                DebugInfo
                    << "\nfacesToPassFluidThrough: "
                    << facesToPassFluidThrough << ", dVftot = "
                    << dVftot << " m3 corresponding to dalpha = "
                    << dVftot/Vi << endl;

                forAll(facesToPassFluidThrough, fi)
                {
                    const label facei = facesToPassFluidThrough[fi];
                    scalar fluidToPassThroughFace =
                        fluidToPassOn*mag(phi[fi]*dt)/dVftot;

                    nFacesToPassFluidThrough +=
                        pos0(dVfmax[fi] - fluidToPassThroughFace);

                    fluidToPassThroughFace =
                        min(fluidToPassThroughFace, dVfmax[fi]);

                    scalar dVff = faceValue(dVf, facei);
                    dVff += sign(phi[fi])*fluidToPassThroughFace;
                    setFaceValue(dVf, facei, dVff);

                    if (firstLoop)
                    {
                        checkIfOnProcPatch(facei);
                        correctedFaces.append(facei);
                    }
                }

                firstLoop = false;
                alpha1New = alpha1[celli] - netFlux(dVf, celli)/Vi;
                alphaOvershoot = alpha1New - 1.0;
                fluidToPassOn = alphaOvershoot*Vi;

                DebugInfo
                    << "\nNew alpha for cell " << celli << ": "
                    << alpha1New << endl;
            }
        }
    }

    DebugInfo << "correctedFaces = " << correctedFaces << endl;
}


Foam::scalar Foam::isoAdvection::netFlux
(
    const surfaceScalarField& dVf,
    const label celli
) const
{
    scalar dV = 0;

    // Get face indices
    const cell& c = mesh_.cells()[celli];

    // Get mesh data
    const labelList& own = mesh_.faceOwner();

    forAll(c, fi)
    {
        const label facei = c[fi];
        const scalar dVff = faceValue(dVf, facei);

        if (own[facei] == celli)
        {
            dV += dVff;
        }
        else
        {
            dV -= dVff;
        }
    }

    return dV;
}


void Foam::isoAdvection::syncProcPatches
(
    surfaceScalarField& dVf,
    const surfaceScalarField& phi
)
{
    const polyBoundaryMesh& patches = mesh_.boundaryMesh();

    if (Pstream::parRun())
    {
        PstreamBuffers pBufs(Pstream::commsTypes::nonBlocking);

        // Send
        forAll(procPatchLabels_, i)
        {
            const label patchi = procPatchLabels_[i];

            const processorPolyPatch& procPatch =
                refCast<const processorPolyPatch>(patches[patchi]);

            UOPstream toNbr(procPatch.neighbProcNo(), pBufs);
            const scalarField& pFlux = dVf.boundaryField()[patchi];

            const List<label>& surfCellFacesOnProcPatch =
                surfaceCellFacesOnProcPatches_[patchi];

            const UIndirectList<scalar> dVfPatch
            (
                pFlux,
                surfCellFacesOnProcPatch
            );

            toNbr << surfCellFacesOnProcPatch << dVfPatch;
        }

        pBufs.finishedSends();


        // Receive and combine
        forAll(procPatchLabels_, patchLabeli)
        {
            const label patchi = procPatchLabels_[patchLabeli];

            const processorPolyPatch& procPatch =
                refCast<const processorPolyPatch>(patches[patchi]);

            UIPstream fromNeighb(procPatch.neighbProcNo(), pBufs);
            List<label> faceIDs;
            List<scalar> nbrdVfs;

            fromNeighb >> faceIDs >> nbrdVfs;

            if (debug)
            {
                Pout<< "Received at time = " << mesh_.time().value()
                    << ": surfCellFacesOnProcPatch = " << faceIDs << nl
                    << "Received at time = " << mesh_.time().value()
                    << ": dVfPatch = " << nbrdVfs << endl;
            }

            // Combine fluxes
            scalarField& localFlux = dVf.boundaryFieldRef()[patchi];

            forAll(faceIDs, i)
            {
                const label facei = faceIDs[i];
                localFlux[facei] = - nbrdVfs[i];
                if (debug && mag(localFlux[facei] + nbrdVfs[i]) > 10*SMALL)
                {
                    Pout<< "localFlux[facei] = " << localFlux[facei]
                        << " and nbrdVfs[i] = " << nbrdVfs[i]
                        << " for facei = " << facei << endl;
                }
            }
        }

        if (debug)
        {
            // Write out results for checking
            forAll(procPatchLabels_, patchLabeli)
            {
                const label patchi = procPatchLabels_[patchLabeli];
                const scalarField& localFlux = dVf.boundaryField()[patchi];
                Pout<< "time = " << mesh_.time().value() << ": localFlux = "
                    << localFlux << endl;
            }
        }

        // Reinitialising list used for minimal parallel communication
        forAll(surfaceCellFacesOnProcPatches_, patchi)
        {
            surfaceCellFacesOnProcPatches_[patchi].clear();
        }
    }
}


void Foam::isoAdvection::checkIfOnProcPatch(const label facei)
{
    if (!mesh_.isInternalFace(facei))
    {
        const polyBoundaryMesh& pbm = mesh_.boundaryMesh();
        const label patchi = pbm.patchID()[facei - mesh_.nInternalFaces()];

        if (isA<processorPolyPatch>(pbm[patchi]) && pbm[patchi].size())
        {
            const label patchFacei = pbm[patchi].whichFace(facei);
            surfaceCellFacesOnProcPatches_[patchi].append(patchFacei);
        }
    }
}


void Foam::isoAdvection::advect()
{
    DebugInFunction << endl;
    
    scalar advectionStartTime = mesh_.time().elapsedCpuTime();

    // Initialising dVf with upwind values
    // i.e. phi[facei]*alpha1[upwindCell[facei]]*dt
    dVf_ = upwind<scalar>(mesh_, phi_).flux(alpha1_)*mesh_.time().deltaT();

    // Do the isoAdvection on surface cells
    timeIntegratedFlux();

    // Adjust alpha for mesh motion
    if (mesh_.moving())
    {
        alpha1In_ *= (mesh_.Vsc0()/mesh_.Vsc());
    }

    // Adjust dVf for unbounded cells
    limitFluxes();

    // Advect the free surface
    alpha1_ -= fvc::surfaceIntegrate(dVf_);
    alpha1_.correctBoundaryConditions();

    scalar maxAlphaMinus1 = gMax(alpha1In_) - 1;
    scalar minAlpha = gMin(alpha1In_);
    Info << "isoAdvection: After conservative bounding: min(alpha) = "
        << minAlpha << ", max(alpha) = 1 + " << maxAlphaMinus1 << endl;

    // Apply non-conservative bounding mechanisms (clipping and snapping)
    // Note: We should be able to write out alpha before this is done!
    applyBruteForceBounding();

    // Write surface cell set and bound cell set if required by user
    writeSurfaceCells();
    writeBoundedCells();

    advectionTime_ += (mesh_.time().elapsedCpuTime() - advectionStartTime);
    Info << "isoAdvection: time consumption = "
        << label(100*advectionTime_/(mesh_.time().elapsedCpuTime() + SMALL))
        << "%" << endl;
}


void Foam::isoAdvection::applyBruteForceBounding()
{
    bool alpha1Changed = false;

    scalar snapAlphaTol = dict_.lookupOrDefault<scalar>("snapTol", 0);
    if (snapAlphaTol > 0)
    {
        alpha1_ =
            alpha1_
           *pos0(alpha1_ - snapAlphaTol)
           *neg0(alpha1_ - (1.0 - snapAlphaTol))
          + pos0(alpha1_ - (1.0 - snapAlphaTol));

        alpha1Changed = true;
    }

    bool clip = dict_.lookupOrDefault<bool>("clip", true);
    if (clip)
    {
        alpha1_ = min(scalar(1.0), max(scalar(0.0), alpha1_));
        alpha1Changed = true;
    }

    if (alpha1Changed)
    {
        alpha1_.correctBoundaryConditions();
    }
}


void Foam::isoAdvection::writeSurfaceCells() const
{
    if (!mesh_.time().writeTime()) return;

    if (dict_.lookupOrDefault<bool>("writeSurfCells", false))
    {
        cellSet cSet
        (
            IOobject
            (
                "surfCells",
                mesh_.time().timeName(),
                mesh_,
                IOobject::NO_READ
            )
        );

        forAll(surfCells_, i)
        {
            cSet.insert(surfCells_[i]);
        }

        cSet.write();
    }
}


void Foam::isoAdvection::writeBoundedCells() const
{
    if (!mesh_.time().writeTime()) return;

    if (dict_.lookupOrDefault<bool>("writeBoundedCells", false))
    {
        cellSet cSet
        (
            IOobject
            (
                "boundedCells",
                mesh_.time().timeName(),
                mesh_,
                IOobject::NO_READ
            )
        );

        forAll(cellIsBounded_, i)
        {
            if (cellIsBounded_[i])
            {
                cSet.insert(i);
            }
        }

        cSet.write();
    }
}


void Foam::isoAdvection::writeIsoFaces
(
    const DynamicList<List<point> >& faces
) const
{

    if (!writeIsoFacesToFile_ || !mesh_.time().writeTime()) return;

    // Writing isofaces to obj file for inspection, e.g. in paraview
    const fileName dirName
    (
        Pstream::parRun() ?
            mesh_.time().path()/".."/"isoFaces"
          : mesh_.time().path()/"isoFaces"
    );
    const string fName
    (
        "isoFaces_" + Foam::name(mesh_.time().timeIndex())
        // Changed because only OF+ has two parameter version of Foam::name
        // "isoFaces_" + Foam::name("%012d", mesh_.time().timeIndex())
    );

    if (Pstream::parRun())
    {
        // Collect points from all the processors
        List<DynamicList<List<point> > > allProcFaces(Pstream::nProcs());
        allProcFaces[Pstream::myProcNo()] = faces;
        Pstream::gatherList(allProcFaces);

        if (Pstream::master())
        {
            mkDir(dirName);
            OBJstream os(dirName/fName + ".obj");
            Info<< nl << "isoAdvection: writing iso faces to file: "
                << os.name() << nl << endl;

            face f;
            forAll(allProcFaces, proci)
            {
                const DynamicList<List<point> >& procFacePts =
                    allProcFaces[proci];

                forAll(procFacePts, i)
                {
                    const List<point>& facePts = procFacePts[i];

                    if (facePts.size() != f.size())
                    {
                        f = face(identity(facePts.size()));
                    }

                    os.write(f, facePts, false);
                }
            }
        }
    }
    else
    {
        mkDir(dirName);
        OBJstream os(dirName/fName + ".obj");
        Info<< nl << "isoAdvection: writing iso faces to file: "
            << os.name() << nl << endl;

        face f;
        forAll(faces, i)
        {
            const List<point>& facePts = faces[i];

            if (facePts.size() != f.size())
            {
                f = face(identity(facePts.size()));
            }

            os.write(f, facePts, false);
        }
    }
}

bool Foam::isoAdvection::cellIsCut(const label celli)
{
    // Calculate cell status (-1: cell is fully below the isosurface, 0:
    // cell is cut, 1: cell is fully above the isosurface)
    label maxIter = 100;
    label cellStatus = isoCutCell_.vofCutCell
    (
        celli,
        alpha1In_[celli],
        isoFaceTol_,
        maxIter
    );

    return cellStatus == 0 ? true : false;
}


// ************************************************************************* //
