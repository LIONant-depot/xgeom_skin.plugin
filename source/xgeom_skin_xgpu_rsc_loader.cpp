#include "xgeom_skin_xgpu_runtime.h"
#include "xgeom_skin_xgpu_rsc_loader.h"

#include "dependencies/xresource_guid/source/bridges/xresource_xproperty_bridge.h"

//
// We will register the loader, the properties,
//
inline static auto s_GeomSkinRegistrations = xresource::common_registrations<xrsc::geom_skin_type_guid_v>{};


//------------------------------------------------------------------

static
std::size_t SkinMultipleOf64(std::size_t num)
{
    return (num + 63) & ~63;
}

//------------------------------------------------------------------

template<typename T>
std::size_t SkinMakeMultipleOf64(const T& X)
{
    return X.size();
    auto s = sizeof(X[0]);
    return SkinMultipleOf64(s * X.size())/ s;
}

//------------------------------------------------------------------

xresource::loader< xrsc::geom_skin_type_guid_v >::data_type* xresource::loader< xrsc::geom_skin_type_guid_v >::Load(xresource::mgr& Mgr, const full_guid& GUID)
{
    auto&                   UserData    = Mgr.getUserData<resource_mgr_user_data>();
    std::wstring            Path        = Mgr.getResourcePath(GUID, type_name_v);
    xgeom_skin::geom*       pGeom       = nullptr;

    // Load the xgeom_skin. A missing/not-yet-compiled resource is an expected, recoverable case (same
    // reasoning as xtexture_xgpu_rsc_loader.cpp's identical fix) - every caller already handles
    // getResource() returning null, so return null instead of asserting-then-dereferencing a null
    // pGeom below (a crash in a debug build, silent undefined behaviour in Release).
    xserializer::stream Stream;
    if (auto Err = Stream.Load(Path, pGeom); Err)
    {
        return nullptr;
    }

    //
    // Time to copy the memory to the right places
    //

    // Upgrade to the runtime version
    xgeom_skin::xgpu::geom* pXGPUGeom = static_cast<xgeom_skin::xgpu::geom*>(pGeom);

    // A corrupt bone addressing reads bone matrices past the skeleton on the GPU: check it before the upload (Debug)
    #ifndef NDEBUG
    {
        std::size_t nBones = 0;
        auto        SkeletonRef = pXGPUGeom->getSkeletonRef();
        if (not SkeletonRef.empty())
        {
            if (auto* pSkeleton = Mgr.getResource(SkeletonRef); pSkeleton)
            {
                nBones = pSkeleton->getBones().size();
                Mgr.ReleaseRef(SkeletonRef);
            }
        }
        if (const char* pErr = pXGPUGeom->ValidateBoneAddressing(nBones); pErr)
        {
            assert(false && "Corrupt xgeom_skin bone addressing");
            xserializer::default_memory_handler_v.Free(xserializer::mem_type{ .m_bUnique = true }, pXGPUGeom);
            return nullptr;
        }
    }
    #endif

    // Create buffers
    xgpu::device::error* p;

    // Ideally in the future all these buffer should be a single allocation...
    0
    ||(p = UserData.m_Device.Create(pXGPUGeom->VertexBuffer(),       xgpu::buffer::setup{.m_Type = xgpu::buffer::type::VERTEX,  .m_EntryByteSize = (int)sizeof(xgeom_skin::geom::vertex),            .m_EntryCount = (int)pXGPUGeom->getVertices().size(),     .m_pData = pXGPUGeom->getVertices().data()}))
    ||(p = UserData.m_Device.Create(pXGPUGeom->VertexExtrasBuffer(), xgpu::buffer::setup{.m_Type = xgpu::buffer::type::VERTEX,  .m_EntryByteSize = (int)sizeof(xgeom_skin::geom::vertex_extras),     .m_EntryCount = (int)pXGPUGeom->getVertexExtras().size(), .m_pData = pXGPUGeom->getVertexExtras().data()}))
    ||(p = UserData.m_Device.Create(pXGPUGeom->IndexBuffer(),        xgpu::buffer::setup{.m_Type = xgpu::buffer::type::INDEX,   .m_EntryByteSize = (int)sizeof(std::uint16_t),                       .m_EntryCount = (int)pXGPUGeom->getIndices().size(),      .m_pData = pXGPUGeom->getIndices().data()}))
    ||(p = UserData.m_Device.Create(pXGPUGeom->ClusterBuffer(),      xgpu::buffer::setup{.m_Type = xgpu::buffer::type::STORAGE, .m_EntryByteSize = (int)sizeof(xgeom_skin::geom::cluster_data),      .m_EntryCount = (int)pXGPUGeom->getClusterData().size(),  .m_pData = pXGPUGeom->getClusterData().data() }))
    ||(pXGPUGeom->getVertexColors().size() && (p = UserData.m_Device.Create(pXGPUGeom->ColorBuffer(), xgpu::buffer::setup{.m_Type = xgpu::buffer::type::VERTEX, .m_EntryByteSize = (int)sizeof(std::uint32_t), .m_EntryCount = (int)pXGPUGeom->getVertexColors().size(), .m_pData = pXGPUGeom->getVertexColors().data()})))
    ;
    assert(p == nullptr);

    // The palettes, as 32 bit words (two uint16 entries each). A skin without palettes binds one shared dummy: the descriptor layout never changes
    if (pXGPUGeom->m_nBoneMap)
    {
        std::vector<std::uint32_t> Words((pXGPUGeom->m_nBoneMap + 1) / 2, 0u);
        std::memcpy(Words.data(), pXGPUGeom->m_pBoneMap, pXGPUGeom->m_nBoneMap * sizeof(std::uint16_t));
        p = UserData.m_Device.Create(pXGPUGeom->BoneMapBuffer(), xgpu::buffer::setup{.m_Type = xgpu::buffer::type::STORAGE, .m_EntryByteSize = (int)sizeof(std::uint32_t), .m_EntryCount = (int)Words.size(), .m_pData = Words.data()});
        assert(p == nullptr);
    }
    else
    {
        static xgpu::buffer s_DummyBoneMap = [&]
        {
            xgpu::buffer        B;
            std::uint32_t       Zero = 0;
            [[maybe_unused]] auto* e = UserData.m_Device.Create(B, xgpu::buffer::setup{.m_Type = xgpu::buffer::type::STORAGE, .m_EntryByteSize = (int)sizeof(std::uint32_t), .m_EntryCount = 1, .m_pData = &Zero});
            assert(e == nullptr);
            return B;
        }();
        pXGPUGeom->BoneMapBuffer() = s_DummyBoneMap;
    }

    // Resolve the default material instances. A referenced material instance that fails to load
    // (missing/uncompiled itself, or a broken dependency of its own) is the same expected,
    // recoverable case as the geometry's own file above - this slot just stays unresolved, same as
    // an empty ref (see the "no default material" comment above); every consumer already renders a
    // fallback (e.g. the preview's own m_White default) rather than assuming every slot resolved.
    for (auto& E : pXGPUGeom->getDefaultMaterialInstances())
    {
        if (not E.empty())
        {
            (void)Mgr.getResource(E);
        }
        else
        {
            // The user will have to deal with no-default materials...
        }
    }

    return pXGPUGeom;
}

//------------------------------------------------------------------

void xresource::loader< xrsc::geom_skin_type_guid_v >::Destroy(xresource::mgr& Mgr, data_type&& Data, const full_guid& GUID)
{
    auto& UserData = Mgr.getUserData<resource_mgr_user_data>();

    // Release all the buffers (all 4 - xgeom_static's own loader omits ClusterBuffer here, which
    // looks like a leak; not propagating that omission into this new plugin)
    UserData.m_Device.Destroy(std::move(Data.VertexBuffer()));
    UserData.m_Device.Destroy(std::move(Data.VertexExtrasBuffer()));
    UserData.m_Device.Destroy(std::move(Data.IndexBuffer()));
    UserData.m_Device.Destroy(std::move(Data.ClusterBuffer()));
    UserData.m_Device.Destroy(std::move(Data.BoneMapBuffer()));     // the shared dummy survives: the device only frees a buffer's last reference
    if (Data.ColorBuffer().m_Private) UserData.m_Device.Destroy(std::move(Data.ColorBuffer()));

    // Release all the material instance references
    for (auto& E : Data.getDefaultMaterialInstances())
    {
        Mgr.ReleaseRef(E);
    }

    // Free the resource
    xserializer::default_memory_handler_v.Free(xserializer::mem_type{ .m_bUnique = true }, &Data);
}
