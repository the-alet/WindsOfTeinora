#include "Rhi/Streamline.h"

#include "Core/Log.h"

#if TEINORA_WITH_STREAMLINE

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_security.h>

#include <string>

namespace Teinora::Rhi
{
	namespace
	{
		constexpr const char* ProjectId = "7a3c1e52-4b8d-4f6a-9c21-5e0d7b3f8a14";
		constexpr const char* EngineVersion = "0.2";

		// The interposer is loaded at run time, so that the program starts without it.
		struct Api
		{
			HMODULE Module = nullptr;
			decltype(&::slInit) Init = nullptr;
			decltype(&::slShutdown) Shutdown = nullptr;
			decltype(&::slIsFeatureSupported) IsFeatureSupported = nullptr;
			decltype(&::slSetD3DDevice) SetD3DDevice = nullptr;
			decltype(&::slGetNativeInterface) GetNativeInterface = nullptr;
			decltype(&::slGetFeatureFunction) GetFeatureFunction = nullptr;
			decltype(&::slGetNewFrameToken) GetNewFrameToken = nullptr;
			decltype(&::slSetTagForFrame) SetTagForFrame = nullptr;
			decltype(&::slSetConstants) SetConstants = nullptr;
			decltype(&::slEvaluateFeature) EvaluateFeature = nullptr;
		};

		Api Loaded;

		template <typename Function>
		void LoadExport(Function& Out, const char* Name)
		{
			Out = reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(Loaded.Module, Name)));
		}

		void OnLogMessage(sl::LogType Type, const char* Text)
		{
			if (Type != sl::LogType::eInfo)
			{
				std::fprintf(stdout, "Streamline: %s", Text);
				std::fflush(stdout);
			}
		}

		sl::DLSSMode ToDlss(EUpscaleMode Mode)
		{
			switch (Mode)
			{
			case EUpscaleMode::Native:           return sl::DLSSMode::eDLAA;
			case EUpscaleMode::Quality:          return sl::DLSSMode::eMaxQuality;
			case EUpscaleMode::Balanced:         return sl::DLSSMode::eBalanced;
			case EUpscaleMode::Performance:      return sl::DLSSMode::eMaxPerformance;
			case EUpscaleMode::UltraPerformance: return sl::DLSSMode::eUltraPerformance;
			default:                             return sl::DLSSMode::eOff;
			}
		}

		// Streamline multiplies row vectors, the renderer column vectors, so matrices flip.
		sl::float4x4 ToStreamline(const float Source[4][4])
		{
			sl::float4x4 Result;
			for (int Row = 0; Row < 4; ++Row)
			{
				Result.setRow(static_cast<std::uint32_t>(Row), { Source[0][Row], Source[1][Row], Source[2][Row], Source[3][Row] });
			}
			return Result;
		}

		sl::float3 ToStreamline(const float Source[3])
		{
			return { Source[0], Source[1], Source[2] };
		}

		sl::Resource MakeResource(const NativeTexture& Texture)
		{
			sl::Resource Result(sl::ResourceType::eTex2d, Texture.Resource, Texture.Memory, Texture.View, Texture.State);
			Result.width = Texture.Width;
			Result.height = Texture.Height;
			Result.nativeFormat = Texture.Format;
			Result.mipLevels = 1;
			Result.arrayLayers = 1;
			Result.flags = 0;
			Result.usage = Texture.Usage;
			return Result;
		}

		class StreamlineFeatures final : public VendorFeatures
		{
		public:
			explicit StreamlineFeatures(Device& InOwner) : Owner(InOwner) {}

			~StreamlineFeatures() override
			{
				Loaded.Shutdown();
			}

			bool Init(const StreamlineAdapter& Adapter)
			{
				sl::AdapterInfo Info{};
				Info.deviceLUID = static_cast<std::uint8_t*>(const_cast<void*>(Adapter.Luid));
				Info.deviceLUIDSizeInBytes = Adapter.LuidSize;
				Info.vkPhysicalDevice = Adapter.PhysicalDevice;

				bUpscaling = Loaded.IsFeatureSupported(sl::kFeatureDLSS, Info) == sl::Result::eOk;
				if (bUpscaling)
				{
					GetFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", GetOptimalSettings);
					GetFunction(sl::kFeatureDLSS, "slDLSSSetOptions", SetDlssOptions);
					bUpscaling = GetOptimalSettings != nullptr && SetDlssOptions != nullptr;
				}

				if (Loaded.IsFeatureSupported(sl::kFeatureReflex, Info) == sl::Result::eOk)
				{
					GetFunction(sl::kFeatureReflex, "slReflexGetState", GetReflexState);
					GetFunction(sl::kFeatureReflex, "slReflexSetOptions", SetReflexOptions);
					GetFunction(sl::kFeatureReflex, "slReflexSleep", ReflexSleep);
				}
				if (Loaded.IsFeatureSupported(sl::kFeaturePCL, Info) == sl::Result::eOk)
				{
					GetFunction(sl::kFeaturePCL, "slPCLSetMarker", SetMarker);
				}

				if (GetReflexState != nullptr && SetReflexOptions != nullptr && ReflexSleep != nullptr)
				{
					sl::ReflexState State{};
					bLowLatency = GetReflexState(State) == sl::Result::eOk && State.lowLatencyAvailable;
					SetLowLatency(ELowLatencyMode::Off); // required once even when off
				}

				Log("Streamline: DLSS %s, Reflex %s", bUpscaling ? "available" : "not available", bLowLatency ? "available" : "not available");
				return bUpscaling || bLowLatency;
			}

			const char* GetUpscalerName() const override { return "DLSS"; }
			bool HasUpscaling() const override { return bUpscaling; }
			bool HasLowLatency() const override { return bLowLatency; }

			void SetLowLatency(ELowLatencyMode Mode) override
			{
				if (SetReflexOptions != nullptr)
				{
					sl::ReflexOptions Options{};
					Options.mode = Mode == ELowLatencyMode::Boost ? sl::ReflexMode::eLowLatencyWithBoost
						: (Mode == ELowLatencyMode::On ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff);
					SetReflexOptions(Options);
				}
			}

			void BeginSimulation() override
			{
				Loaded.GetNewFrameToken(Token, nullptr);
				if (ReflexSleep != nullptr)
				{
					ReflexSleep(*Token);
				}
				Mark(sl::PCLMarker::eSimulationStart);
			}

			void EndSimulation() override { Mark(sl::PCLMarker::eSimulationEnd); }
			void BeginRenderSubmit() override { Mark(sl::PCLMarker::eRenderSubmitStart); }
			void EndRenderSubmit() override { Mark(sl::PCLMarker::eRenderSubmitEnd); }
			void BeginPresent() override { Mark(sl::PCLMarker::ePresentStart); }
			void EndPresent() override { Mark(sl::PCLMarker::ePresentEnd); }

			bool GetRenderSize(EUpscaleMode Mode, std::uint32_t OutputWidth, std::uint32_t OutputHeight, std::uint32_t& OutWidth, std::uint32_t& OutHeight) override
			{
				sl::DLSSOptions Options{};
				Options.mode = ToDlss(Mode);
				Options.outputWidth = OutputWidth;
				Options.outputHeight = OutputHeight;

				sl::DLSSOptimalSettings Optimal{};
				if (!bUpscaling || GetOptimalSettings(Options, Optimal) != sl::Result::eOk || Optimal.optimalRenderWidth == 0)
				{
					return false;
				}
				OutWidth = Optimal.optimalRenderWidth;
				OutHeight = Optimal.optimalRenderHeight;
				return true;
			}

			void Upscale(const UpscaleInputs& Inputs) override
			{
				if (Token == nullptr)
				{
					return;
				}

				sl::Constants Constants{};
				Constants.cameraViewToClip = ToStreamline(Inputs.ViewToClip);
				Constants.clipToCameraView = ToStreamline(Inputs.ClipToView);
				Constants.clipToPrevClip = ToStreamline(Inputs.ClipToPrevClip);
				Constants.prevClipToClip = ToStreamline(Inputs.PrevClipToClip);
				Constants.jitterOffset = { Inputs.JitterX, Inputs.JitterY };
				Constants.mvecScale = { 1.0f, 1.0f }; // motion is already in UV units
				Constants.cameraPinholeOffset = { 0.0f, 0.0f };
				Constants.cameraPos = { 0.0f, 0.0f, 0.0f }; // everything is relative to the camera
				Constants.cameraFwd = ToStreamline(Inputs.Forward);
				Constants.cameraRight = ToStreamline(Inputs.Right);
				Constants.cameraUp = ToStreamline(Inputs.Up);
				Constants.cameraNear = Inputs.NearPlane;
				Constants.cameraFar = 100000.0f; // the projection is infinite, so any large distance will do
				Constants.cameraFOV = Inputs.VerticalFov;
				Constants.cameraAspectRatio = Inputs.Aspect;
				Constants.depthInverted = sl::Boolean::eTrue;
				Constants.cameraMotionIncluded = sl::Boolean::eTrue;
				Constants.motionVectors3D = sl::Boolean::eFalse;
				Constants.reset = Inputs.bReset ? sl::Boolean::eTrue : sl::Boolean::eFalse;

				const sl::ViewportHandle Viewport{ 0u };
				Loaded.SetConstants(Constants, *Token, Viewport);

				sl::DLSSOptions Options{};
				Options.mode = ToDlss(Inputs.Mode);
				Options.outputWidth = Inputs.OutputWidth;
				Options.outputHeight = Inputs.OutputHeight;
				Options.colorBuffersHDR = sl::Boolean::eTrue;
				Options.useAutoExposure = sl::Boolean::eTrue;
				SetDlssOptions(Viewport, Options);

				sl::Resource Color = MakeResource(Owner.AccessForExternal(Inputs.Color, false));
				sl::Resource Depth = MakeResource(Owner.AccessForExternal(Inputs.Depth, false));
				sl::Resource Motion = MakeResource(Owner.AccessForExternal(Inputs.Motion, false));
				sl::Resource Output = MakeResource(Owner.AccessForExternal(Inputs.Output, true));

				const sl::Extent RenderArea{ 0, 0, Inputs.RenderWidth, Inputs.RenderHeight };
				const sl::Extent OutputArea{ 0, 0, Inputs.OutputWidth, Inputs.OutputHeight };
				const sl::ResourceTag Tags[] = {
					sl::ResourceTag(&Color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &RenderArea),
					sl::ResourceTag(&Depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &RenderArea),
					sl::ResourceTag(&Motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &RenderArea),
					sl::ResourceTag(&Output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &OutputArea) };

				sl::CommandBuffer* Commands = static_cast<sl::CommandBuffer*>(Owner.GetNativeCommandList());
				Loaded.SetTagForFrame(*Token, Viewport, Tags, 4, Commands);

				const sl::BaseStructure* EvaluateInputs[] = { &Viewport };
				const sl::Result Result = Loaded.EvaluateFeature(sl::kFeatureDLSS, *Token, EvaluateInputs, 1, Commands);
				if (Result != sl::Result::eOk && !bReportedFailure)
				{
					bReportedFailure = true;
					Log("Streamline: DLSS evaluation failed: %d", static_cast<int>(Result));
				}
			}

		private:
			template <typename Function>
			void GetFunction(sl::Feature Feature, const char* Name, Function& Out)
			{
				void* Pointer = nullptr;
				if (Loaded.GetFeatureFunction(Feature, Name, Pointer) == sl::Result::eOk)
				{
					Out = reinterpret_cast<Function>(Pointer);
				}
			}

			void Mark(sl::PCLMarker Marker)
			{
				if (SetMarker != nullptr && Token != nullptr)
				{
					SetMarker(Marker, *Token);
				}
			}

			Device& Owner;
			sl::FrameToken* Token = nullptr;
			bool bUpscaling = false;
			bool bLowLatency = false;
			bool bReportedFailure = false;

			PFun_slDLSSGetOptimalSettings* GetOptimalSettings = nullptr;
			PFun_slDLSSSetOptions* SetDlssOptions = nullptr;
			PFun_slReflexGetState* GetReflexState = nullptr;
			PFun_slReflexSetOptions* SetReflexOptions = nullptr;
			PFun_slReflexSleep* ReflexSleep = nullptr;
			PFun_slPCLSetMarker* SetMarker = nullptr;
		};
	}

	bool StartStreamline(EBackend Backend)
	{
		wchar_t Path[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, Path, MAX_PATH);
		std::wstring Interposer = Path;
		Interposer = Interposer.substr(0, Interposer.find_last_of(L"\\/") + 1) + L"sl.interposer.dll";

		if (GetFileAttributesW(Interposer.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			return false;
		}
		if (!sl::security::verifyEmbeddedSignature(Interposer.c_str()))
		{
			Log("Streamline: sl.interposer.dll is not signed by NVIDIA, ignoring it");
			return false;
		}

		Loaded.Module = LoadLibraryW(Interposer.c_str());
		if (Loaded.Module == nullptr)
		{
			return false;
		}
		LoadExport(Loaded.Init, "slInit");
		LoadExport(Loaded.Shutdown, "slShutdown");
		LoadExport(Loaded.IsFeatureSupported, "slIsFeatureSupported");
		LoadExport(Loaded.SetD3DDevice, "slSetD3DDevice");
		LoadExport(Loaded.GetNativeInterface, "slGetNativeInterface");
		LoadExport(Loaded.GetFeatureFunction, "slGetFeatureFunction");
		LoadExport(Loaded.GetNewFrameToken, "slGetNewFrameToken");
		LoadExport(Loaded.SetTagForFrame, "slSetTagForFrame");
		LoadExport(Loaded.SetConstants, "slSetConstants");
		LoadExport(Loaded.EvaluateFeature, "slEvaluateFeature");

		const sl::Feature Features[] = { sl::kFeatureDLSS, sl::kFeatureReflex, sl::kFeaturePCL };

		sl::Preferences Preferences{};
		Preferences.logLevel = sl::LogLevel::eDefault;
		Preferences.logMessageCallback = &OnLogMessage;
		Preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging; // no over-the-air updates
		Preferences.featuresToLoad = Features;
		Preferences.numFeaturesToLoad = 3;
		Preferences.engine = sl::EngineType::eCustom;
		Preferences.engineVersion = EngineVersion;
		Preferences.projectId = ProjectId;
		Preferences.renderAPI = Backend == EBackend::Vulkan ? sl::RenderAPI::eVulkan : sl::RenderAPI::eD3D12;

		const sl::Result Result = Loaded.Init != nullptr ? Loaded.Init(Preferences, sl::kSDKVersion) : sl::Result::eErrorNotInitialized;
		if (Result != sl::Result::eOk)
		{
			Log("Streamline: slInit failed: %d", static_cast<int>(Result));
			Loaded.Module = nullptr;
			return false;
		}
		return true;
	}

	void* GetStreamlineExport(const char* Name)
	{
		return Loaded.Module != nullptr ? reinterpret_cast<void*>(GetProcAddress(Loaded.Module, Name)) : nullptr;
	}

	void SetStreamlineDevice(void* NativeDevice)
	{
		if (Loaded.Module == nullptr)
		{
			return;
		}

		// The device handed to us is a proxy, and Streamline wants the real one.
		void* Native = nullptr;
		Loaded.GetNativeInterface(NativeDevice, &Native);
		Loaded.SetD3DDevice(Native != nullptr ? Native : NativeDevice);
	}

	std::unique_ptr<VendorFeatures> CreateStreamlineFeatures(Device& Owner, const StreamlineAdapter& Adapter)
	{
		auto Result = std::make_unique<StreamlineFeatures>(Owner);
		if (!Result->Init(Adapter))
		{
			return nullptr;
		}
		return Result;
	}
}

#else

namespace Teinora::Rhi
{
	bool StartStreamline(EBackend)
	{
		return false;
	}

	void* GetStreamlineExport(const char*)
	{
		return nullptr;
	}

	void SetStreamlineDevice(void*)
	{
	}

	std::unique_ptr<VendorFeatures> CreateStreamlineFeatures(Device&, const StreamlineAdapter&)
	{
		return nullptr;
	}
}

#endif
