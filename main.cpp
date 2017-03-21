/************************************************************************************
Authors     :   Bruno Evangelista
Copyright   :   Copyright 2016 Oculus VR, LLC All Rights reserved.

Licensed under the Oculus VR Rift SDK License Version 3.3 (the "License"); 
you may not use the Oculus VR Rift SDK except in compliance with the License, 
which is provided at the time of installation or download, or which 
otherwise accompanies this software in either electronic or hard copy form.

You may obtain a copy of the License at
http://www.oculusvr.com/licenses/LICENSE-3.3 

Unless required by applicable law or agreed to in writing, the Oculus VR SDK 
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*************************************************************************************/

#define   OVR_D3D_VERSION 11
#pragma warning(disable: 4324)
#include "Win32_DirectXAppUtil.h" // DirectX Helper
#include "OVR_CAPI_D3D.h" // Oculus SDK
#include "openvr.h"
#include <chrono>
DirectX11 DIRECTX;
typedef std::chrono::time_point<std::chrono::high_resolution_clock> high_resolution_clock;
float randVelocity() { return (rand() % 201) * 0.01f - 1.0f; }
float randColor() { return (rand() % 81 + 20) * 0.01f; };


class GuardianSystemDemo
{
public:
    void Start(HINSTANCE hinst);
    void InitRenderTargets(const ovrHmdDesc& hmdDesc);
    void InitSceneGraph();

    float UpdateTimeWithBoundaryTest();
    void  UpdateBoundaryLookAndFeel();
    void  UpdateObjectsCollisionWithBoundary(float elapsedTimeSec);
    void  Render();

private:
    XMVECTOR mObjPosition[Scene::MAX_MODELS];                               // Objects cached position 
    XMVECTOR mObjVelocity[Scene::MAX_MODELS];                               // Objects velocity
    Scene mDynamicScene;                                                    // Scene graph

    ovrSession mSession = nullptr;
    high_resolution_clock mLastUpdateClock;                                 // Stores last update time
    float mGlobalTimeSec = 0;                                               // Game global time

    uint32_t mFrameIndex = 0;                                               // Global frame counter
    ovrVector3f mHmdToEyeOffset[ovrEye_Count] = {};                         // Offset from the center of the HMD to each eye
    ovrRecti mEyeRenderViewport[ovrEye_Count] = {};                         // Eye render target viewport

    ovrLayerEyeFov mEyeRenderLayer = {};                                    // OVR  - Eye render layers description
    ovrTextureSwapChain mTextureChain[ovrEye_Count] = {};                   // OVR  - Eye render target swap chain
    ID3D11DepthStencilView* mEyeDepthTarget[ovrEye_Count] = {};             // DX11 - Eye depth view
    std::vector<ID3D11RenderTargetView*> mEyeRenderTargets[ovrEye_Count];   // DX11 - Eye render view
	std::vector<ovrVector3f> mPlayPoints;
	std::vector<ovrTrackerPose> mTrackerPoses;

    bool mShouldQuit = false;
    bool mSlowMotionMode = false;                                           // Slow motion gets enabled when too close to the boundary
};


void GuardianSystemDemo::InitRenderTargets(const ovrHmdDesc& hmdDesc)
{
    // For each eye
    for (int i = 0; i < ovrEye_Count; ++i) {
        // Viewport
        const float kPixelsPerDisplayPixel = 1.0f;
        ovrSizei idealSize = ovr_GetFovTextureSize(mSession, (ovrEyeType)i, hmdDesc.DefaultEyeFov[i], kPixelsPerDisplayPixel);
        mEyeRenderViewport[i] = { 0, 0, idealSize.w, idealSize.h };

        // Create Swap Chain
        ovrTextureSwapChainDesc desc = {
            ovrTexture_2D, OVR_FORMAT_R8G8B8A8_UNORM_SRGB, 1, idealSize.w, idealSize.h, 1, 1, 
            ovrFalse, ovrTextureMisc_DX_Typeless, ovrTextureBind_DX_RenderTarget
        };

        // Configure Eye render layers
        mEyeRenderLayer.Header.Type = ovrLayerType_EyeFov;
        mEyeRenderLayer.Viewport[i] = mEyeRenderViewport[i];
        mEyeRenderLayer.Fov[i] = hmdDesc.DefaultEyeFov[i];
        mHmdToEyeOffset[i] = ovr_GetRenderDesc(mSession, (ovrEyeType)i, hmdDesc.DefaultEyeFov[i]).HmdToEyeOffset;

        // DirectX 11 - Generate RenderTargetView from textures in swap chain
        // ----------------------------------------------------------------------
        ovrResult result = ovr_CreateTextureSwapChainDX(mSession, DIRECTX.Device, &desc, &mTextureChain[i]);
        if (!OVR_SUCCESS(result)) {
            printf("ovr_CreateTextureSwapChainDX failed"); exit(-1);
        }

        // Render Target, normally triple-buffered
        int textureCount = 0;
        ovr_GetTextureSwapChainLength(mSession, mTextureChain[i], &textureCount);
        for (int j = 0; j < textureCount; ++j) {
            ID3D11Texture2D* renderTexture = nullptr;
            ovr_GetTextureSwapChainBufferDX(mSession, mTextureChain[i], j, IID_PPV_ARGS(&renderTexture));
            
            D3D11_RENDER_TARGET_VIEW_DESC renderTargetViewDesc = {
                DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_RTV_DIMENSION_TEXTURE2D
            };

            ID3D11RenderTargetView* renderTargetView = nullptr;
            DIRECTX.Device->CreateRenderTargetView(renderTexture, &renderTargetViewDesc, &renderTargetView);
            mEyeRenderTargets[i].push_back(renderTargetView);
            renderTexture->Release();
        }

        // DirectX 11 - Generate Depth
        // ----------------------------------------------------------------------
        D3D11_TEXTURE2D_DESC depthTextureDesc = {
            (UINT)idealSize.w, (UINT)idealSize.h, 1, 1, DXGI_FORMAT_D32_FLOAT, {1, 0},
            D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL, 0, 0
        };
        
        ID3D11Texture2D* depthTexture = nullptr;
        DIRECTX.Device->CreateTexture2D(&depthTextureDesc, NULL, &depthTexture);
        DIRECTX.Device->CreateDepthStencilView(depthTexture, NULL, &mEyeDepthTarget[i]);
        depthTexture->Release();
    }
}


void GuardianSystemDemo::InitSceneGraph()
{
    for (int32_t i = 0; i < Scene::MAX_MODELS; ++i) {
        TriangleSet mesh;
        mesh.AddSolidColorBox(-0.035f, -0.035f, -0.035f, 0.035f, 0.035f, 0.035f, 0xFFFFFFFF);

        // Objects start 1 meter high
        mObjPosition[i] = XMVectorSet(0.0f, 1.0f, 0.0f, 1.0f);
        // Objects have random velocity
        mObjVelocity[i] = XMVectorSet(randVelocity(), randVelocity() * 0.5f, randVelocity(), 0);
        mObjVelocity[i] = XMVector3Normalize(mObjVelocity[i]) * 0.3f;

        Material* mat = new Material(new Texture(false, 256, 256, Texture::AUTO_FLOOR));
        mDynamicScene.Add(new Model(&mesh, XMFLOAT3(0, 0, 0), XMFLOAT4(0, 0, 0, 1), mat));
    }
}


void GuardianSystemDemo::Start(HINSTANCE hinst)
{


    ovrResult result;
    result = ovr_Initialize(nullptr);
    if (!OVR_SUCCESS(result)) {
        printf("ovr_Initialize failed"); exit(-1);
    }

    ovrGraphicsLuid luid;
    result = ovr_Create(&mSession, &luid);
    if (!OVR_SUCCESS(result)) {
        printf("ovr_Create failed"); exit(-1);
    }

    if (!DIRECTX.InitWindow(hinst, L"GuardianSystemDemo")) {
        printf("DIRECTX.InitWindow failed"); exit(-1);
    }

    // Use HMD desc to initialize device
    ovrHmdDesc hmdDesc = ovr_GetHmdDesc(mSession);
    if (!DIRECTX.InitDevice(hmdDesc.Resolution.w / 2, hmdDesc.Resolution.h / 2, reinterpret_cast<LUID*>(&luid))) {
        printf("DIRECTX.InitDevice failed"); exit(-1);
    }

    // Use FloorLevel tracking origin
    //ovr_SetTrackingOriginType(mSession, ovrTrackingOrigin_FloorLevel);
	ovr_SetTrackingOriginType(mSession, ovrTrackingOrigin_EyeLevel);

	int numOfPlayPoints;
	if (!OVR_SUCCESS(ovr_GetBoundaryGeometry(mSession, ovrBoundaryType::ovrBoundary_PlayArea, NULL, &numOfPlayPoints))) {
		printf("Getting number of boundary points failed"); exit(-1);
	}

	mPlayPoints.resize(numOfPlayPoints);
	if (!OVR_SUCCESS(ovr_GetBoundaryGeometry(mSession, ovrBoundaryType::ovrBoundary_PlayArea, mPlayPoints.data(), &numOfPlayPoints))) {
		printf("Getting boundary points failed"); exit(-1);
	}

	int numOfGuardianPoints;
	std::vector<ovrVector3f> guardianPoints;
	if (!OVR_SUCCESS(ovr_GetBoundaryGeometry(mSession, ovrBoundaryType::ovrBoundary_Outer, NULL, &numOfGuardianPoints))) {
		printf("Getting number of guardian points failed"); exit(-1);
	}

	guardianPoints.resize(numOfGuardianPoints);

	if (!OVR_SUCCESS(ovr_GetBoundaryGeometry(mSession, ovrBoundaryType::ovrBoundary_Outer, guardianPoints.data(), &numOfGuardianPoints))) {
		printf("Getting guardian points failed"); exit(-1);
	}

	ovrVector3f dimensions;
	if (!OVR_SUCCESS(ovr_GetBoundaryDimensions(mSession, ovrBoundaryType::ovrBoundary_PlayArea, &dimensions))) {
		printf("Getting boundary dimensions failed"); exit(-1);
	}

	ovrVector3f origin;

	origin.x = 0;
	origin.y = 0;
	origin.z = 0;

	for (unsigned int i = 0; i < mPlayPoints.size(); ++i) {
		origin.x += mPlayPoints[i].x;
		origin.y += mPlayPoints[i].y;
		origin.z += mPlayPoints[i].z;
	}

	int numOfPoints = mPlayPoints.size();
	origin.x = origin.x / (float)numOfPoints;
	origin.y = origin.y / (float)numOfPoints;
	origin.z = origin.z / (float)numOfPoints;

	//origin.y *= -1; // I don't know why they're upside down relative to each other.

	printf("%f, %f, %f", origin.x, origin.y, origin.z);

	unsigned int numOfTrackers = ovr_GetTrackerCount(mSession);
	mTrackerPoses.resize(numOfTrackers);
	for (unsigned int i = 0; i < numOfTrackers; i++) {
		ovrTrackerPose trackerPose = ovr_GetTrackerPose(mSession, i);
		mTrackerPoses[i] = trackerPose;
	}

	//ovr_Shutdown();

	vr::EVRInitError initError;
	vr::IVRSystem* ivrSystem = vr::VR_Init(&initError, vr::EVRApplicationType::VRApplication_Scene);

	std::vector<vr::TrackedDevicePose_t> poses;

	poses.resize(vr::k_unMaxTrackedDeviceCount);

	ivrSystem->GetDeviceToAbsoluteTrackingPose(vr::ETrackingUniverseOrigin::TrackingUniverseRawAndUncalibrated, 0.0, poses.data(), vr::k_unMaxTrackedDeviceCount);

	std::vector<vr::TrackedDevicePose_t> cameraPoses;

	for (unsigned int i = 0; i < poses.size(); ++i) {
		if (ivrSystem->GetTrackedDeviceClass(i) == vr::TrackedDeviceClass_TrackingReference) {
			cameraPoses.push_back(poses[i]);
		}
	}


	auto chaperoneStatus = vr::VRChaperone()->GetCalibrationState(); // REQUIRED in order to do any chaperone setup

	//auto trackingSpace = vr::VRCompositor()->GetTrackingSpace();

	vr::VRChaperoneSetup()->RevertWorkingCopy();
	vr::HmdMatrix34_t standingZero = {};
	//vr::VRChaperoneSetup()->GetWorkingStandingZeroPoseToRawTrackingPose(&standingZero);
	for (unsigned int i = 0; i < 3; i++) {
		for (unsigned int j = 0; j < 4; j++) {
			//standingZero.m[i][j] = 0;
		}
	}
	//standingZero.m[1][3] = 5;
	// Rotation to 0, 1s along diagonal I think
	standingZero.m[0][0] = 1;
	standingZero.m[1][1] = 1;
	standingZero.m[2][2] = 1;

	//Position
	standingZero.m[0][3] = origin.x;
	standingZero.m[1][3] = origin.y;
	standingZero.m[2][3] = origin.z;

	uint32_t quadCount = 0;
	vr::VRChaperoneSetup()->GetWorkingCollisionBoundsInfo(NULL, &quadCount);

	std::vector<vr::HmdQuad_t> quads(numOfGuardianPoints);

	for (unsigned int i = 0; i < numOfGuardianPoints; i++) {
		unsigned int j = (i + 1) % numOfGuardianPoints;
		quads[i].vCorners[0].v[0] = guardianPoints[i].x - origin.x;
		quads[i].vCorners[0].v[1] = guardianPoints[i].y - origin.y;
		quads[i].vCorners[0].v[2] = guardianPoints[i].z - origin.z;

		quads[i].vCorners[1].v[0] = guardianPoints[i].x - origin.x;
		quads[i].vCorners[1].v[1] = guardianPoints[i].y - origin.y + 2.43;
		quads[i].vCorners[1].v[2] = guardianPoints[i].z - origin.z;

		quads[i].vCorners[2].v[0] = guardianPoints[j].x - origin.x;
		quads[i].vCorners[2].v[1] = guardianPoints[j].y - origin.y + 2.43;
		quads[i].vCorners[2].v[2] = guardianPoints[j].z - origin.z;

		quads[i].vCorners[3].v[0] = guardianPoints[j].x - origin.x;
		quads[i].vCorners[3].v[1] = guardianPoints[j].y - origin.y;
		quads[i].vCorners[3].v[2] = guardianPoints[j].z - origin.z;
		
	}


	
	vr::VRChaperoneSetup()->SetWorkingStandingZeroPoseToRawTrackingPose(&standingZero);
	vr::VRChaperoneSetup()->SetWorkingPlayAreaSize(dimensions.x, dimensions.z);
	quads.resize(0);
	vr::VRChaperoneSetup()->SetWorkingCollisionBoundsInfo(quads.data(), numOfGuardianPoints);
	vr::VRChaperoneSetup()->CommitWorkingCopy(vr::EChaperoneConfigFile_Live);
	//vr::VRChaperoneSetup()->ReloadFromDisk(vr::EChaperoneConfigFile_Live);
	//vr::VRChaperone()->ReloadInfo();
	//

	//vr::HmdMatrix34_t poseMatrix;
	//bool getPoseResult = vr::VRChaperoneSetup()->GetWorkingStandingZeroPoseToRawTrackingPose(&poseMatrix);
	//quads.clear();
	//vr::VRChaperoneSetup()->SetWorkingCollisionBoundsInfo(quads.data(), 0);
	//vr::VRChaperoneSetup()->ReloadFromDisk(vr::EChaperoneConfigFile_Live);
	//vr::VRChaperoneSetup()->SetWorkingStandingZeroPoseToRawTrackingPose


	exit(0);

    
    InitRenderTargets(hmdDesc);
    InitSceneGraph();
    mLastUpdateClock = std::chrono::high_resolution_clock::now();

    // Main Loop
    while (DIRECTX.HandleMessages() && !mShouldQuit)
    {
        ovrSessionStatus sessionStatus;
        ovr_GetSessionStatus(mSession, &sessionStatus);
        if (sessionStatus.ShouldQuit)
            break;

        float elapsedTimeSec = UpdateTimeWithBoundaryTest();
        UpdateBoundaryLookAndFeel();
        UpdateObjectsCollisionWithBoundary(elapsedTimeSec);
        Render();
    }


    ovr_Shutdown();


}


float GuardianSystemDemo::UpdateTimeWithBoundaryTest()
{
    // Calculate elapsed time
    auto clockNow = std::chrono::high_resolution_clock::now();
    float elapsedTimeSec = ((std::chrono::duration<float>)(clockNow - mLastUpdateClock)).count();
    mLastUpdateClock = clockNow;

    // Check if ANY tracked device is triggering the outer boundary
    ovrBoundaryTestResult test;
    ovr_TestBoundary(mSession, ovrTrackedDevice_All, ovrBoundary_Outer, &test);

    const float kSlowMotionStartDistance = 0.5f;    // Slow motion start at half a meter
    const float kStopMotionDistance = 0.1f;         // Stops motion at 10cm
    const float kMotionDistanceRange = kSlowMotionStartDistance - kStopMotionDistance;
    if (test.ClosestDistance < kSlowMotionStartDistance) {
        //elapsedTimeSec *= (max(0.0f, test.ClosestDistance - kStopMotionDistance) / kMotionDistanceRange);
    }

    mGlobalTimeSec += elapsedTimeSec;
    srand((uint32_t)mGlobalTimeSec);

    return elapsedTimeSec;
}


void GuardianSystemDemo::UpdateBoundaryLookAndFeel()
{
    if ((uint32_t)mGlobalTimeSec % 2 == 1) {
        ovrBoundaryLookAndFeel lookAndFeel = {};
        lookAndFeel.Color = { randColor(), randColor(), randColor(), 1.0f };
        ovr_SetBoundaryLookAndFeel(mSession, &lookAndFeel);
        ovr_RequestBoundaryVisible(mSession, ovrTrue);
    }
    else {
        ovr_ResetBoundaryLookAndFeel(mSession);
        ovr_RequestBoundaryVisible(mSession, ovrFalse);
    }
}


void GuardianSystemDemo::UpdateObjectsCollisionWithBoundary(float elapsedTimeSec)
{
    if (mGlobalTimeSec < 1.0f) return; // Start update after 1s

    for (int32_t i = 0; i < mDynamicScene.numModels; ++i) {
        XMFLOAT3 newPosition;
		XMVECTOR newPositionVec;
		if ((size_t)i < mPlayPoints.size()) {
			newPosition.x = mPlayPoints[i].x;
			newPosition.y = mPlayPoints[i].y;
			newPosition.z = mPlayPoints[i].z;
			newPositionVec = XMLoadFloat3(&newPosition);
		}
		else {
			newPositionVec = XMVectorAdd(mObjPosition[i], XMVectorScale(mObjVelocity[i], elapsedTimeSec));
			XMStoreFloat3(&newPosition, newPositionVec);
		}


        // Test object collision with boundary
        ovrBoundaryTestResult test;
        ovr_TestBoundaryPoint(mSession, (ovrVector3f*)&newPosition.x, ovrBoundary_Outer, &test);

        // Collides with surface at 2cm
        //if (test.ClosestDistance < 0.02f) {
		if (FALSE) {
            XMVECTOR surfaceNormal = XMVectorSet(test.ClosestPointNormal.x, test.ClosestPointNormal.y, test.ClosestPointNormal.z, 0.0f);
            mObjVelocity[i] = XMVector3Reflect(mObjVelocity[i], surfaceNormal);

            newPositionVec = XMVectorAdd(mObjPosition[i], XMVectorScale(mObjVelocity[i], elapsedTimeSec));
            XMStoreFloat3(&newPosition, newPositionVec);
        }

        mObjPosition[i] = newPositionVec;
        mDynamicScene.Models[i]->Pos = newPosition;
    }
}


void GuardianSystemDemo::Render()
{
    // Get current eye pose for rendering
    double eyePoseTime = 0;
    ovrPosef eyePose[ovrEye_Count] = {};
    ovr_GetEyePoses(mSession, mFrameIndex, ovrTrue, mHmdToEyeOffset, eyePose, &eyePoseTime);

    // Render each eye
    for (int i = 0; i < ovrEye_Count; ++i) {
        int renderTargetIndex = 0;
        ovr_GetTextureSwapChainCurrentIndex(mSession, mTextureChain[i], &renderTargetIndex);
        ID3D11RenderTargetView* renderTargetView = mEyeRenderTargets[i][renderTargetIndex];
        ID3D11DepthStencilView* depthTargetView = mEyeDepthTarget[i];

        // Clear and set render/depth target and viewport
        DIRECTX.SetAndClearRenderTarget(renderTargetView, depthTargetView, 0.2f, 0.2f, 0.2f, 1.0f);
        DIRECTX.SetViewport((float)mEyeRenderViewport[i].Pos.x, (float)mEyeRenderViewport[i].Pos.y, 
            (float)mEyeRenderViewport[i].Size.w, (float)mEyeRenderViewport[i].Size.h);

        // Eye
        XMVECTOR eyeRot = XMVectorSet(eyePose[i].Orientation.x, eyePose[i].Orientation.y, 
            eyePose[i].Orientation.z, eyePose[i].Orientation.w);
        XMVECTOR eyePos = XMVectorSet(eyePose[i].Position.x, eyePose[i].Position.y, eyePose[i].Position.z, 0);
        XMVECTOR eyeForward = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), eyeRot);

        // Matrices
        XMMATRIX viewMat = XMMatrixLookAtRH(eyePos, XMVectorAdd(eyePos, eyeForward), 
            XMVector3Rotate(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), eyeRot));
        ovrMatrix4f proj = ovrMatrix4f_Projection(mEyeRenderLayer.Fov[i], 0.001f, 1000.0f, ovrProjection_None);
        XMMATRIX projMat = XMMatrixTranspose(XMMATRIX(&proj.M[0][0]));
        XMMATRIX viewProjMat = XMMatrixMultiply(viewMat, projMat);

        // Render and commit to swap chain
        mDynamicScene.Render(&viewProjMat, 1.0f, 1.0f, 1.0f, 1.0f, true);
        ovr_CommitTextureSwapChain(mSession, mTextureChain[i]);

        // Update eye layer
        mEyeRenderLayer.ColorTexture[i] = mTextureChain[i];
        mEyeRenderLayer.RenderPose[i] = eyePose[i];
        mEyeRenderLayer.SensorSampleTime = eyePoseTime;
    }

    // Submit frames
    ovrLayerHeader* layers = &mEyeRenderLayer.Header;
    ovrResult result = ovr_SubmitFrame(mSession, mFrameIndex++, nullptr, &layers, 1);
    if (!OVR_SUCCESS(result)) {
        printf("ovr_SubmitFrame failed"); exit(-1);
    }
}


int WINAPI WinMain(HINSTANCE hinst, HINSTANCE, LPSTR, int)
{
    GuardianSystemDemo* instance = new (_aligned_malloc(sizeof(GuardianSystemDemo), 16)) GuardianSystemDemo();
    instance->Start(hinst);
    delete instance;
    return 0;
}
