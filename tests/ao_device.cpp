// Real D3D9/D3DX43 smoke test. Run from src/hlsl/NewVegas/Effects.
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <wrl/client.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
static const char* CurrentStage="startup";
static void Check(HRESULT hr) {
    if (FAILED(hr)) { char text[160]; sprintf_s(text, "D3D call failed at %s: %08lx", CurrentStage, hr); throw std::runtime_error(text); }
}
static ComPtr<IDirect3DTexture9> ConstantTexture(IDirect3DDevice9* device, float r, float g, float b) {
    ComPtr<IDirect3DTexture9> texture;
    Check(device->CreateTexture(1, 1, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &texture, NULL));
    D3DLOCKED_RECT lock;
    Check(texture->LockRect(0, &lock, NULL, 0));
    float* pixels = static_cast<float*>(lock.pBits);
    pixels[0] = r; pixels[1] = g; pixels[2] = b; pixels[3] = 1;
    Check(texture->UnlockRect(0));
    return texture;
}
static void ReadQuery(IDirect3DQuery9* query, void* value, DWORD size) {
    ULONGLONG deadline=GetTickCount64()+10000;
    HRESULT hr;
    while((hr=query->GetData(value,size,D3DGETDATA_FLUSH))==S_FALSE) {
        if(GetTickCount64()>deadline) throw std::runtime_error("GPU query timed out");
        Sleep(1);
    }
    Check(hr);
}
static void Run(IDirect3DDevice9* device, ID3DXEffect* effect, UINT width, UINT height, float depth, int mode=1, UINT frames=1) {
    const char* modeName=mode==2?"DedicatedAO":mode==1?"PackedAO":"LegacyAO";
    CurrentStage="create run textures";
    ComPtr<IDirect3DTexture9> target, rendered, source, ao[2];
    ComPtr<IDirect3DSurface9> rt, rs, ss, aoSurface[2], readback;
    Check(device->CreateTexture(width,height,1,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,D3DPOOL_DEFAULT,&target,NULL));
    Check(device->CreateTexture(width,height,1,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,D3DPOOL_DEFAULT,&rendered,NULL));
    Check(device->CreateTexture(width,height,1,D3DUSAGE_RENDERTARGET,D3DFMT_A16B16G16R16F,D3DPOOL_DEFAULT,&source,NULL));
    Check(target->GetSurfaceLevel(0,&rt)); Check(rendered->GetSurfaceLevel(0,&rs)); Check(source->GetSurfaceLevel(0,&ss));
    UINT lowW=(width+1)/2,lowH=(height+1)/2;
    if(mode==2) for(int i=0;i<2;++i) {
        Check(device->CreateTexture(lowW,lowH,1,D3DUSAGE_RENDERTARGET,D3DFMT_G16R16F,D3DPOOL_DEFAULT,&ao[i],NULL));
        Check(ao[i]->GetSurfaceLevel(0,&aoSurface[i]));
    }
    Check(device->SetRenderTarget(0,rt.Get()));
    Check(device->SetDepthStencilSurface(NULL));
    Check(device->SetRenderState(D3DRS_ZENABLE,FALSE));
    Check(device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE));
    Check(device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));
    Check(device->Clear(0,NULL,D3DCLEAR_TARGET,D3DCOLOR_XRGB(64,128,192),1,0));
    Check(device->StretchRect(rt.Get(),NULL,ss.Get(),NULL,D3DTEXF_NONE));
    // Poison unused scratch space. Edge clamping must prevent these values leaking in.
    Check(device->SetRenderTarget(0,rs.Get()));
    Check(device->Clear(0,NULL,D3DCLEAR_TARGET,D3DCOLOR_XRGB(0,255,255),1,0));
    Check(device->SetRenderTarget(0,rt.Get()));
    const float farZ=10000, q=farZ/(farZ-1);
    auto depths=ConstantTexture(device,depth/farZ,q*(1-1/depth),0);
    auto normals=ConstantTexture(device,0.5f,0.5f,0);
    auto noise=ConstantTexture(device,0.7f,0.4f,0.6f);
    Check(device->SetTexture(0,rendered.Get())); Check(device->SetTexture(1,depths.Get()));
    Check(device->SetTexture(2,source.Get())); Check(device->SetTexture(3,noise.Get())); Check(device->SetTexture(4,normals.Get()));
    for (DWORD i=0;i<5;++i) {
        Check(device->SetSamplerState(i,D3DSAMP_MINFILTER,D3DTEXF_POINT));
        Check(device->SetSamplerState(i,D3DSAMP_MAGFILTER,D3DTEXF_POINT));
        Check(device->SetSamplerState(i,D3DSAMP_MIPFILTER,D3DTEXF_NONE));
        Check(device->SetSamplerState(i,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));
        Check(device->SetSamplerState(i,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));
    }
    auto vector=[&](const char* name,float x,float y,float z,float w) { D3DXVECTOR4 v(x,y,z,w); Check(effect->SetVector(name,&v)); };
    vector("TESR_ReciprocalResolution",1.0f/width,1.0f/height,0,0);
    vector("TESR_CameraData",1,farZ,0,0); vector("TESR_DepthConstants",0,0,0,0);
    vector("TESR_AmbientOcclusionAOData",5,1,0.2f,30); vector("TESR_AmbientOcclusionData",0,0.7f,8,2);
    vector("TESR_FogData",2000,8000,0,0); vector("TESR_FogColor",1,1,1,1);
    D3DXMATRIX proj,inv; D3DXMatrixPerspectiveFovLH(&proj,1.0f,float(width)/height,1,farZ); D3DXMatrixInverse(&inv,NULL,&proj);
    Check(effect->SetMatrix("TESR_ProjectionTransform",&proj)); Check(effect->SetMatrix("TESR_InvProjectionTransform",&inv));
    vector("NVR_AOLayout",float(lowW)/width,float(lowH)/height,1.0f/lowW,1.0f/lowH);
    struct Vertex { float x,y,z,u,v; };
    float ox=0.5f/width,oy=0.5f/height;
    Vertex vertices[]={{-1,1,1,ox,oy},{-1,-1,1,ox,1+oy},{1,1,1,1+ox,oy},{1,-1,1,1+ox,1+oy}};
    Check(device->SetFVF(D3DFVF_XYZ|D3DFVF_TEX1));
    D3DXHANDLE technique=mode==2?effect->GetTechniqueByName("DedicatedAO"):
        mode==1?effect->GetTechniqueByName("PackedAO"):effect->GetTechnique(0);
    Check(effect->SetTechnique(technique));
    CurrentStage="validate technique";
    Check(effect->ValidateTechnique(technique));
    ComPtr<IDirect3DQuery9> start,end,frequency,disjoint;
    if(frames>1) {
        Check(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP,&start)); Check(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP,&end));
        Check(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ,&frequency)); Check(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT,&disjoint));
        Check(disjoint->Issue(D3DISSUE_BEGIN)); Check(start->Issue(D3DISSUE_END));
    }
    for(UINT frame=0;frame<frames;++frame) {
    CurrentStage="begin scene/effect";
    Check(device->BeginScene()); UINT passes=0; Check(effect->Begin(&passes,0));
    if(passes!=(mode?4u:5u)) throw std::runtime_error("Unexpected pass count");
    RECT rect={0,0,LONG(lowW),LONG(lowH)};
    for(UINT p=0;p<passes;++p) {
        CurrentStage="configure pass";
        bool combine=!mode || p==passes-1;
        if(mode==2) {
            Check(device->SetTexture(5,NULL));
            Check(device->SetRenderTarget(0,combine?rt.Get():aoSurface[p==1?1:0].Get()));
        }
        D3DVIEWPORT9 vp={0,0,combine?width:lowW,combine?height:lowH,0,1};
        Check(device->SetViewport(&vp)); Check(effect->BeginPass(p));
        if(mode==2) {
            if(p==1) Check(device->SetTexture(5,ao[0].Get()));
            else if(p==2) Check(device->SetTexture(5,ao[1].Get()));
            else if(combine) Check(device->SetTexture(5,ao[0].Get()));
        }
        CurrentStage="draw pass";
        Check(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(Vertex)));
        Check(effect->EndPass());
        if(mode!=2) Check(device->StretchRect(rt.Get(),combine?NULL:&rect,rs.Get(),combine?NULL:&rect,D3DTEXF_NONE));
    }
    Check(effect->End()); Check(device->EndScene());
    }
    if(frames>1) {
        Check(end->Issue(D3DISSUE_END)); Check(frequency->Issue(D3DISSUE_END)); Check(disjoint->Issue(D3DISSUE_END));
        UINT64 a=0,b=0,hz=0; BOOL invalid=FALSE;
        ReadQuery(end.Get(),&b,sizeof(b)); ReadQuery(start.Get(),&a,sizeof(a));
        ReadQuery(frequency.Get(),&hz,sizeof(hz)); ReadQuery(disjoint.Get(),&invalid,sizeof(invalid));
        if(invalid || !hz) throw std::runtime_error("Invalid GPU timing interval");
        std::printf("GPU synthetic %s %ux%u: %.4f ms/frame (%u frames)\n",modeName,width,height,double(b-a)*1000.0/double(hz)/frames,frames);
    }
    Check(device->CreateOffscreenPlainSurface(width,height,D3DFMT_A16B16G16R16F,D3DPOOL_SYSTEMMEM,&readback,NULL));
    Check(device->GetRenderTargetData(rt.Get(),readback.Get()));
    D3DLOCKED_RECT lock; Check(readback->LockRect(&lock,NULL,D3DLOCK_READONLY));
    float largest=0;
    for(UINT y=0;y<height;++y) for(UINT x=0;x<width;++x) {
        auto pixel=reinterpret_cast<D3DXFLOAT16*>(static_cast<char*>(lock.pBits)+y*lock.Pitch)+x*4;
        float values[4]; D3DXFloat16To32Array(values,pixel,4);
        for(int c=0;c<3;++c) {
            if(!std::isfinite(values[c])) throw std::runtime_error("Nonfinite pixel");
            float error=std::abs(values[c]-float(64*(c+1))/255);
            if(error>largest) largest=error;
        }
    }
    Check(readback->UnlockRect());
    if(largest>0.006f) throw std::runtime_error("Flat-plane/sky AO altered source or leaked scratch pixels");
    std::printf("PASS D3D9 %s %ux%u depth %.0f, max color error %.6f\n",modeName,width,height,depth,largest);
    for(DWORD i=0;i<6;++i) device->SetTexture(i,NULL);
}
int main(int argc, char** argv) {
    // --compile-files A.fx.hlsl B.fx.hlsl ...: compile each effect with the game's runtime compiler
    // (D3DX9_43), with and without REVERSED_DEPTH, and exit. Run from the Effects folder.
    if (argc >= 3 && std::strcmp(argv[1], "--compile-files") == 0) {
        int failures = 0;
        for (int i = 2; i < argc; ++i) {
            for (int reversed = 0; reversed < 2; ++reversed) {
                D3DXMACRO defines[] = {{reversed ? "REVERSED_DEPTH" : NULL, ""}, {NULL, NULL}};
                ComPtr<ID3DXEffectCompiler> compiler; ComPtr<ID3DXBuffer> err, code;
                HRESULT result = D3DXCreateEffectCompilerFromFileA(argv[i], defines, NULL, 0, &compiler, &err);
                if (SUCCEEDED(result)) { err.Reset(); result = compiler->CompileEffect(0, &code, &err); }
                if (FAILED(result)) {
                    std::printf("FAIL D3DX43 %s (reversed depth %s)\n", argv[i], reversed ? "on" : "off");
                    if (err) std::puts(static_cast<const char*>(err->GetBufferPointer()));
                    ++failures;
                }
            }
            if (!failures) std::printf("PASS D3DX43 %s\n", argv[i]);
        }
        return failures ? 1 : 0;
    }
    try {
        CurrentStage="compile SunShadows variants";
        for(int forward=0;forward<2;++forward) for(int reversed=0;reversed<2;++reversed) {
            D3DXMACRO defines[]={{"FORWARD_SHADOWS",forward?"1":"0"},{reversed?"REVERSED_DEPTH":NULL,""},{NULL,NULL}};
            ComPtr<ID3DXEffectCompiler> sun; ComPtr<ID3DXBuffer> err,code;
            HRESULT result=D3DXCreateEffectCompilerFromFileA("SunShadows.fx.hlsl",defines,NULL,0,&sun,&err);
            if(err) std::puts(static_cast<const char*>(err->GetBufferPointer())); Check(result);
            err.Reset(); result=sun->CompileEffect(0,&code,&err);
            if(err) std::puts(static_cast<const char*>(err->GetBufferPointer())); Check(result);
        }
        std::puts("PASS D3DX43 SunShadows: forward on/off, reversed depth on/off");
        CurrentStage="compile AmbientOcclusion";
        ComPtr<ID3DXEffectCompiler> compiler; ComPtr<ID3DXBuffer> errors,bytecode;
        HRESULT hr=D3DXCreateEffectCompilerFromFileA("AmbientOcclusion.fx.hlsl",NULL,NULL,0,&compiler,&errors);
        if(errors) std::puts(static_cast<const char*>(errors->GetBufferPointer())); Check(hr);
        errors.Reset(); hr=compiler->CompileEffect(0,&bytecode,&errors);
        if(errors) std::puts(static_cast<const char*>(errors->GetBufferPointer())); Check(hr);
        std::puts("PASS D3DX43 effect compilation (runtime compiler)");
        CurrentStage="compile VolumetricFog variants";
        for (int reversed=0; reversed<2; ++reversed) {
            D3DXMACRO defines[]={{reversed?"REVERSED_DEPTH":NULL,""},{NULL,NULL}};
            ComPtr<ID3DXEffectCompiler> fog; ComPtr<ID3DXBuffer> err,code;
            HRESULT result=D3DXCreateEffectCompilerFromFileA("VolumetricFog.fx.hlsl",defines,NULL,0,&fog,&err);
            if(err) std::puts(static_cast<const char*>(err->GetBufferPointer())); Check(result);
            err.Reset(); result=fog->CompileEffect(0,&code,&err);
            if(err) std::puts(static_cast<const char*>(err->GetBufferPointer())); Check(result);
        }
        std::puts("PASS D3DX43 VolumetricFog: all techniques, reversed depth on/off");
        CurrentStage="compile FXAA";
        {
            ComPtr<ID3DXEffectCompiler> fxaa; ComPtr<ID3DXBuffer> err,code;
            HRESULT result=D3DXCreateEffectCompilerFromFileA("FXAA.fx.hlsl",NULL,NULL,0,&fxaa,&err);
            if(err) std::puts(static_cast<const char*>(err->GetBufferPointer())); Check(result);
            err.Reset(); result=fxaa->CompileEffect(0,&code,&err);
            if(err) std::puts(static_cast<const char*>(err->GetBufferPointer())); Check(result);
        }
        std::puts("PASS D3DX43 FXAA");
        if (argc == 2 && std::strcmp(argv[1], "--compile-only") == 0) return 0;
        CurrentStage="create hidden window";
        HWND window=CreateWindowExA(0,"STATIC","NVR hidden AO test",WS_OVERLAPPED,0,0,128,128,NULL,NULL,GetModuleHandle(NULL),NULL);
        if(!window) throw std::runtime_error("Window creation failed");
        CurrentStage="create Direct3D9";
        ComPtr<IDirect3D9> d3d; d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
        if(!d3d) throw std::runtime_error("D3D9 unavailable");
        D3DADAPTER_IDENTIFIER9 adapter={}; Check(d3d->GetAdapterIdentifier(0,0,&adapter)); std::puts(adapter.Description);
        D3DDISPLAYMODE display={}; Check(d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT,&display));
        D3DPRESENT_PARAMETERS pp={}; pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow=window;
        pp.BackBufferWidth=128; pp.BackBufferHeight=128; pp.BackBufferFormat=display.Format;
        pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
        ComPtr<IDirect3DDevice9> device;
        CurrentStage="create D3D9 device";
        HRESULT create=d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device);
        if(FAILED(create)) create=d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&device);
        bool referenceDevice=FAILED(create);
        if(referenceDevice) create=d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_REF,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&device);
        Check(create);
        CurrentStage="create AO effect";
        ComPtr<ID3DXEffect> effect; errors.Reset();
        Check(D3DXCreateEffect(device.Get(),bytecode->GetBufferPointer(),bytecode->GetBufferSize(),NULL,NULL,0,NULL,&effect,&errors));
        Run(device.Get(),effect.Get(),64,48,100);
        Run(device.Get(),effect.Get(),65,49,100);
        Run(device.Get(),effect.Get(),65,49,9000);
        Run(device.Get(),effect.Get(),65,49,100,0);
        Run(device.Get(),effect.Get(),64,48,100,2);
        Run(device.Get(),effect.Get(),65,49,100,2);
        Run(device.Get(),effect.Get(),65,49,9000,2);
        if(!referenceDevice) {
            Run(device.Get(),effect.Get(),1920,1080,100,1,24);
            Run(device.Get(),effect.Get(),1920,1080,100,0,24);
            Run(device.Get(),effect.Get(),1920,1080,100,2,24);
        }
        DestroyWindow(window);
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
