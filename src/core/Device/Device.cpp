#include "Device.h"

// Forwarding bodies for the table in Device.h. Each call goes to the same method on the wrapped device, with the
// same arguments, and returns what that call returns.
#define TESR_D3D9_DEFINE(Ret, Name, Params, Args) \
	STDMETHODIMP_(Ret) TESRDirect3DDevice9::Name(TESR_D3D9_UNPAREN Params) { return D3DDevice->Name Args; }

#define TESR_D3D9_DEFINE_NOTHROW(Ret, Name, Params, Args) \
	COM_DECLSPEC_NOTHROW Ret STDMETHODCALLTYPE TESRDirect3DDevice9::Name(TESR_D3D9_UNPAREN Params) { return D3DDevice->Name Args; }

TESRDirect3DDevice9::TESRDirect3DDevice9(IDirect3D9Ex* Interface9, IDirect3DDevice9Ex* Device9) : IDirect3DDevice9Ex() {
	D3DInterface = Interface9;
	D3DDevice = Device9;
}

TESRDirect3DDevice9::~TESRDirect3DDevice9() = default;

// Drops one reference on the wrapped device; the wrapper goes away with the last one.
STDMETHODIMP_(ULONG) TESRDirect3DDevice9::Release() {
	const ULONG remaining = D3DDevice->Release();
	if (remaining == 0) delete this;
	return remaining;
}

// Reports the wrapper itself as the parent object, so callers get back the interface they were given.
STDMETHODIMP TESRDirect3DDevice9::GetDirect3D(IDirect3D9** ppD3D9) {
	const HRESULT result = D3DDevice->GetDirect3D(ppD3D9);
	if (SUCCEEDED(result)) *ppD3D9 = static_cast<IDirect3D9*>(D3DInterface);
	return result;
}

TESR_D3D9_FORWARDS(TESR_D3D9_DEFINE, TESR_D3D9_DEFINE_NOTHROW)
