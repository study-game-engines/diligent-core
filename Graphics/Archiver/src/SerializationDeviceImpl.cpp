/*
 *  Copyright 2019-2021 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  In no event and under no legal theory, whether in tort (including negligence),
 *  contract, or otherwise, unless required by applicable law (such as deliberate
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental,
 *  or consequential damages of any character arising as a result of this License or
 *  out of the use or inability to use the software (including but not limited to damages
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and
 *  all other commercial damages or losses), even if such Contributor has been advised
 *  of the possibility of such damages.
 */

#include "ArchiverImpl.hpp"
#include "SerializationDeviceImpl.hpp"
#include "GLSLangUtils.hpp"
#include "SerializableShaderImpl.hpp"
#include "SerializableRenderPassImpl.hpp"
#include "SerializableResourceSignatureImpl.hpp"
#include "EngineMemory.h"

namespace Diligent
{
namespace
{
static constexpr Uint32 GetDeviceBits()
{
    Uint32 DeviceBits = 0;
#if D3D11_SUPPORTED
    DeviceBits |= 1 << RENDER_DEVICE_TYPE_D3D11;
#endif
#if D3D12_SUPPORTED
    DeviceBits |= 1 << RENDER_DEVICE_TYPE_D3D12;
#endif
#if GL_SUPPORTED
    DeviceBits |= 1 << RENDER_DEVICE_TYPE_GL;
#endif
#if GLES_SUPPORTED
    DeviceBits |= 1 << RENDER_DEVICE_TYPE_GLES;
#endif
#if VULKAN_SUPPORTED
    DeviceBits |= 1 << RENDER_DEVICE_TYPE_VULKAN;
#endif
#if METAL_SUPPORTED
    DeviceBits |= 1 << RENDER_DEVICE_TYPE_METAL;
#endif
    return DeviceBits;
}

static constexpr Uint32 ValidDeviceBits = GetDeviceBits();

template <typename SignatureType>
using SignatureArray = std::array<RefCntAutoPtr<SignatureType>, MAX_RESOURCE_SIGNATURES>;

template <typename SignatureType>
static void SortResourceSignatures(const PipelineResourceBindingAttribs& Info, SignatureArray<SignatureType>& Signatures, Uint32& SignaturesCount)
{
    for (Uint32 i = 0; i < Info.ResourceSignaturesCount; ++i)
    {
        const auto* pSerPRS = ClassPtrCast<SerializableResourceSignatureImpl>(Info.ppResourceSignatures[i]);
        const auto& Desc    = pSerPRS->GetDesc();

        Signatures[Desc.BindingIndex] = pSerPRS->GetSignature<SignatureType>();
        SignaturesCount               = std::max(SignaturesCount, static_cast<Uint32>(Desc.BindingIndex) + 1);
    }
}
} // namespace


DummyRenderDevice::DummyRenderDevice(IReferenceCounters* pRefCounters, const RenderDeviceInfo& DeviceInfo, const GraphicsAdapterInfo& AdapterInfo) :
    TBase{pRefCounters},
    m_DeviceInfo{DeviceInfo},
    m_AdapterInfo{AdapterInfo}
{
}

DummyRenderDevice::~DummyRenderDevice()
{}


SerializationDeviceImpl::SerializationDeviceImpl(IReferenceCounters* pRefCounters, const SerializationDeviceCreateInfo& CreateInfo) :
    TBase{pRefCounters},
    m_Device{pRefCounters, CreateInfo.DeviceInfo, CreateInfo.AdapterInfo}
{
#if !DILIGENT_NO_GLSLANG
    GLSLangUtils::InitializeGlslang();
#endif

#if D3D11_SUPPORTED
    m_D3D11FeatureLevel = CreateInfo.D3D11.FeatureLevel;
#endif
#if D3D12_SUPPORTED
    m_D3D12ShaderVersion = CreateInfo.D3D12.ShaderVersion;
    m_pDxCompiler        = CreateDXCompiler(DXCompilerTarget::Direct3D12, 0, CreateInfo.D3D12.DxCompilerPath);
#endif
#if VULKAN_SUPPORTED
    m_VkVersion          = CreateInfo.Vulkan.ApiVersion;
    m_VkSupportedSpirv14 = (m_VkVersion >= Version{1, 2} ? true : CreateInfo.Vulkan.SupportedSpirv14);
    m_pVkDxCompiler      = CreateDXCompiler(DXCompilerTarget::Vulkan, GetVkVersion(), CreateInfo.Vulkan.DxCompilerPath);
#endif
#if METAL_SUPPORTED
    m_MtlTempShaderFolder = CreateInfo.Metal.TempShaderFolder ? CreateInfo.Metal.TempShaderFolder : "";
    m_MslPreprocessorCmd  = CreateInfo.Metal.MslPreprocessorCmd ? CreateInfo.Metal.MslPreprocessorCmd : "";
    m_MtlCompileOptions   = CreateInfo.Metal.CompileOptions ? CreateInfo.Metal.CompileOptions : "";
    m_MtlLinkOptions      = CreateInfo.Metal.LinkOptions ? CreateInfo.Metal.LinkOptions : "";
#endif
}

SerializationDeviceImpl::~SerializationDeviceImpl()
{
#if !DILIGENT_NO_GLSLANG
    GLSLangUtils::FinalizeGlslang();
#endif
}

Uint32 SerializationDeviceImpl::GetValidDeviceBits()
{
    return ValidDeviceBits;
}

void SerializationDeviceImpl::CreateShader(const ShaderCreateInfo& ShaderCI, Uint32 DeviceBits, IShader** ppShader)
{
    DEV_CHECK_ERR(ppShader != nullptr, "ppShader must not be null");
    if (!ppShader)
        return;

    *ppShader = nullptr;
    try
    {
        auto& RawMemAllocator = GetRawAllocator();
        auto* pShaderImpl(NEW_RC_OBJ(RawMemAllocator, "Shader instance", SerializableShaderImpl)(this, ShaderCI, DeviceBits));
        pShaderImpl->QueryInterface(IID_Shader, reinterpret_cast<IObject**>(ppShader));
    }
    catch (...)
    {
        LOG_ERROR_MESSAGE("Failed to create the shader");
    }
}

void SerializationDeviceImpl::CreateRenderPass(const RenderPassDesc& Desc, IRenderPass** ppRenderPass)
{
    DEV_CHECK_ERR(ppRenderPass != nullptr, "ppRenderPass must not be null");
    if (!ppRenderPass)
        return;

    *ppRenderPass = nullptr;
    try
    {
        auto& RawMemAllocator = GetRawAllocator();
        auto* pRenderPassImpl(NEW_RC_OBJ(RawMemAllocator, "Render pass instance", SerializableRenderPassImpl)(this, Desc));
        pRenderPassImpl->QueryInterface(IID_RenderPass, reinterpret_cast<IObject**>(ppRenderPass));
    }
    catch (...)
    {
        LOG_ERROR_MESSAGE("Failed to create the render pass");
    }
}

void SerializationDeviceImpl::CreatePipelineResourceSignature(const PipelineResourceSignatureDesc& Desc, Uint32 DeviceBits, IPipelineResourceSignature** ppSignature)
{
    CreatePipelineResourceSignature(Desc, DeviceBits, SHADER_TYPE_UNKNOWN, ppSignature);
}

void SerializationDeviceImpl::CreatePipelineResourceSignature(const PipelineResourceSignatureDesc& Desc,
                                                              Uint32                               DeviceBits,
                                                              SHADER_TYPE                          ShaderStages,
                                                              IPipelineResourceSignature**         ppSignature)
{
    DEV_CHECK_ERR(ppSignature != nullptr, "ppSignature must not be null");
    if (!ppSignature)
        return;

    *ppSignature = nullptr;
    try
    {
        auto& RawMemAllocator = GetRawAllocator();
        auto* pSignatureImpl(NEW_RC_OBJ(RawMemAllocator, "Pipeline resource signature instance", SerializableResourceSignatureImpl)(this, Desc, DeviceBits, ShaderStages));
        pSignatureImpl->QueryInterface(IID_PipelineResourceSignature, reinterpret_cast<IObject**>(ppSignature));
    }
    catch (...)
    {
        LOG_ERROR_MESSAGE("Failed to create the resource signature");
    }
}

void SerializationDeviceImpl::GetPipelineResourceBindings(const PipelineResourceBindingAttribs& Info,
                                                          Uint32&                               NumBindings,
                                                          const PipelineResourceBinding*&       pBindings)
{
    NumBindings = 0;
    pBindings   = nullptr;
    m_ResourceBindings.clear();

    const auto       ShaderStages = (Info.ShaderStages == SHADER_TYPE_UNKNOWN ? static_cast<SHADER_TYPE>(~0u) : Info.ShaderStages);
    constexpr Uint32 RuntimeArray = 0;

    (void)ShaderStages;
    (void)RuntimeArray;

    switch (Info.DeviceType)
    {
#if D3D11_SUPPORTED
        case RENDER_DEVICE_TYPE_D3D11:
        {
            constexpr SHADER_TYPE SupportedStagesMask = (SHADER_TYPE_ALL_GRAPHICS | SHADER_TYPE_COMPUTE);

            SignatureArray<PipelineResourceSignatureD3D11Impl> Signatures      = {};
            Uint32                                             SignaturesCount = 0;
            SortResourceSignatures(Info, Signatures, SignaturesCount);

            D3D11ShaderResourceCounters BaseBindings = {};
            // In Direct3D11, UAVs use the same register space as render targets
            BaseBindings[D3D11_RESOURCE_RANGE_UAV][PSInd] = Info.NumRenderTargets;

            for (Uint32 sign = 0; sign < SignaturesCount; ++sign)
            {
                const PipelineResourceSignatureD3D11Impl* const pSignature = Signatures[sign];
                if (pSignature == nullptr)
                    continue;

                for (Uint32 r = 0; r < pSignature->GetTotalResourceCount(); ++r)
                {
                    const auto& ResDesc = pSignature->GetResourceDesc(r);
                    const auto& ResAttr = pSignature->GetResourceAttribs(r);
                    const auto  Range   = PipelineResourceSignatureD3D11Impl::ShaderResourceTypeToRange(ResDesc.ResourceType);

                    for (auto Stages = ShaderStages & SupportedStagesMask; Stages != 0;)
                    {
                        const auto ShaderStage = ExtractLSB(Stages);
                        const auto ShaderInd   = GetShaderTypeIndex(ShaderStage);

                        if ((ResDesc.ShaderStages & ShaderStage) == 0)
                            continue;

                        VERIFY_EXPR(ResAttr.BindPoints.IsStageActive(ShaderInd));
                        const auto Binding = Uint32{BaseBindings[Range][ShaderInd]} + Uint32{ResAttr.BindPoints[ShaderInd]};

                        PipelineResourceBinding Dst{};
                        Dst.Name         = ResDesc.Name;
                        Dst.ResourceType = ResDesc.ResourceType;
                        Dst.Register     = Binding;
                        Dst.Space        = 0;
                        Dst.ArraySize    = (ResDesc.Flags & PIPELINE_RESOURCE_FLAG_RUNTIME_ARRAY) == 0 ? ResDesc.ArraySize : RuntimeArray;
                        Dst.ShaderStages = ShaderStage;
                        m_ResourceBindings.push_back(Dst);
                    }
                }

                for (Uint32 samp = 0; samp < pSignature->GetImmutableSamplerCount(); ++samp)
                {
                    const auto& ImtblSam = pSignature->GetImmutableSamplerDesc(samp);
                    const auto& SampAttr = pSignature->GetImmutableSamplerAttribs(samp);
                    const auto  Range    = D3D11_RESOURCE_RANGE_SAMPLER;

                    for (auto Stages = ShaderStages & SupportedStagesMask; Stages != 0;)
                    {
                        const auto ShaderStage = ExtractLSB(Stages);
                        const auto ShaderInd   = GetShaderTypeIndex(ShaderStage);

                        if ((ImtblSam.ShaderStages & ShaderStage) == 0)
                            continue;

                        VERIFY_EXPR(SampAttr.BindPoints.IsStageActive(ShaderInd));
                        const auto Binding = Uint32{BaseBindings[Range][ShaderInd]} + Uint32{SampAttr.BindPoints[ShaderInd]};

                        PipelineResourceBinding Dst{};
                        Dst.Name         = ImtblSam.SamplerOrTextureName;
                        Dst.ResourceType = SHADER_RESOURCE_TYPE_SAMPLER;
                        Dst.Register     = Binding;
                        Dst.Space        = 0;
                        Dst.ArraySize    = SampAttr.ArraySize;
                        Dst.ShaderStages = ShaderStage;
                        m_ResourceBindings.push_back(Dst);
                    }
                }
                pSignature->ShiftBindings(BaseBindings);
            }
            break;
        }
#endif
#if D3D12_SUPPORTED
        case RENDER_DEVICE_TYPE_D3D12:
        {
            SignatureArray<PipelineResourceSignatureD3D12Impl> Signatures      = {};
            Uint32                                             SignaturesCount = 0;
            SortResourceSignatures(Info, Signatures, SignaturesCount);

            RootSignatureD3D12 RootSig{nullptr, nullptr, Signatures.data(), SignaturesCount, 0};
            const bool         HasSpaces = RootSig.GetTotalSpaces() > 1;

            for (Uint32 sign = 0; sign < SignaturesCount; ++sign)
            {
                const auto& pSignature = Signatures[sign];
                if (pSignature == nullptr)
                    continue;

                const auto BaseRegisterSpace = RootSig.GetBaseRegisterSpace(sign);

                for (Uint32 r = 0; r < pSignature->GetTotalResourceCount(); ++r)
                {
                    const auto& ResDesc = pSignature->GetResourceDesc(r);
                    const auto& ResAttr = pSignature->GetResourceAttribs(r);

                    if ((ResDesc.ShaderStages & ShaderStages) == 0)
                        continue;

                    PipelineResourceBinding Dst{};
                    Dst.Name         = ResDesc.Name;
                    Dst.ResourceType = ResDesc.ResourceType;
                    Dst.Register     = ResAttr.Register;
                    Dst.Space        = StaticCast<Uint16>(BaseRegisterSpace + ResAttr.Space);
                    Dst.ArraySize    = (ResDesc.Flags & PIPELINE_RESOURCE_FLAG_RUNTIME_ARRAY) == 0 ? ResDesc.ArraySize : RuntimeArray;
                    Dst.ShaderStages = ResDesc.ShaderStages;
                    m_ResourceBindings.push_back(Dst);
                }
            }
            break;
        }
#endif
#if GL_SUPPORTED || GLES_SUPPORTED
        case RENDER_DEVICE_TYPE_GL:
        case RENDER_DEVICE_TYPE_GLES:
        {
            constexpr SHADER_TYPE SupportedStagesMask = (SHADER_TYPE_ALL_GRAPHICS | SHADER_TYPE_COMPUTE);

            SignatureArray<PipelineResourceSignatureGLImpl> Signatures      = {};
            Uint32                                          SignaturesCount = 0;
            SortResourceSignatures(Info, Signatures, SignaturesCount);

            PipelineResourceSignatureGLImpl::TBindings BaseBindings = {};
            for (Uint32 s = 0; s < SignaturesCount; ++s)
            {
                const auto& pSignature = Signatures[s];
                if (pSignature == nullptr)
                    continue;

                for (Uint32 r = 0; r < pSignature->GetTotalResourceCount(); ++r)
                {
                    const auto& ResDesc = pSignature->GetResourceDesc(r);
                    const auto& ResAttr = pSignature->GetResourceAttribs(r);
                    const auto  Range   = PipelineResourceToBindingRange(ResDesc);

                    for (auto Stages = ShaderStages & SupportedStagesMask; Stages != 0;)
                    {
                        const auto ShaderStage = ExtractLSB(Stages);

                        if ((ResDesc.ShaderStages & ShaderStage) == 0)
                            continue;

                        PipelineResourceBinding Dst{};
                        Dst.Name         = ResDesc.Name;
                        Dst.ResourceType = ResDesc.ResourceType;
                        Dst.Register     = BaseBindings[Range] + ResAttr.CacheOffset;
                        Dst.Space        = 0;
                        Dst.ArraySize    = (ResDesc.Flags & PIPELINE_RESOURCE_FLAG_RUNTIME_ARRAY) == 0 ? ResDesc.ArraySize : RuntimeArray;
                        Dst.ShaderStages = ShaderStage;
                        m_ResourceBindings.push_back(Dst);
                    }
                }
                pSignature->ShiftBindings(BaseBindings);
            }
            break;
        }
#endif
#if VULKAN_SUPPORTED
        case RENDER_DEVICE_TYPE_VULKAN:
        {
            SignatureArray<PipelineResourceSignatureVkImpl> Signatures      = {};
            Uint32                                          SignaturesCount = 0;
            SortResourceSignatures(Info, Signatures, SignaturesCount);

            Uint32 DescSetLayoutCount = 0;
            for (Uint32 sign = 0; sign < SignaturesCount; ++sign)
            {
                const auto& pSignature = Signatures[sign];
                if (pSignature == nullptr)
                    continue;

                for (Uint32 r = 0; r < pSignature->GetTotalResourceCount(); ++r)
                {
                    const auto& ResDesc = pSignature->GetResourceDesc(r);
                    const auto& ResAttr = pSignature->GetResourceAttribs(r);

                    if ((ResDesc.ShaderStages & ShaderStages) == 0)
                        continue;

                    PipelineResourceBinding Dst{};
                    Dst.Name         = ResDesc.Name;
                    Dst.ResourceType = ResDesc.ResourceType;
                    Dst.Register     = ResAttr.BindingIndex;
                    Dst.Space        = StaticCast<Uint16>(DescSetLayoutCount + ResAttr.DescrSet);
                    Dst.ArraySize    = (ResDesc.Flags & PIPELINE_RESOURCE_FLAG_RUNTIME_ARRAY) == 0 ? ResDesc.ArraySize : RuntimeArray;
                    Dst.ShaderStages = ResDesc.ShaderStages;
                    m_ResourceBindings.push_back(Dst);
                }

                // Same as PipelineLayoutVk::Create()
                for (auto SetId : {PipelineResourceSignatureVkImpl::DESCRIPTOR_SET_ID_STATIC_MUTABLE, PipelineResourceSignatureVkImpl::DESCRIPTOR_SET_ID_DYNAMIC})
                {
                    if (pSignature->GetDescriptorSetSize(SetId) != ~0u)
                        ++DescSetLayoutCount;
                }
            }
            VERIFY_EXPR(DescSetLayoutCount <= MAX_RESOURCE_SIGNATURES * 2);
            VERIFY_EXPR(DescSetLayoutCount >= Info.ResourceSignaturesCount);
            break;
        }
#endif
#if METAL_SUPPORTED
        case RENDER_DEVICE_TYPE_METAL:
        {
            GetMetalPipelineResourceBindings(Info, m_ResourceBindings, MtlMaxBufferFunctionArgumets());
            break;
        }
#endif

        case RENDER_DEVICE_TYPE_UNDEFINED:
        case RENDER_DEVICE_TYPE_COUNT:
        default:
            return;
    }

    NumBindings = static_cast<Uint32>(m_ResourceBindings.size());
    pBindings   = m_ResourceBindings.data();
}

} // namespace Diligent