#pragma once

// Addresses of game vtables that NVR code compares object vtables against (it uses the numbers directly, with a
// comment pointing here). Not included anywhere; kept as the reference for those numbers. Add entries as needed.
enum ClassVtblAddress : UInt32 {
	// image space modifier instances (TES::activeIMODs)
	kVtbl_ImageSpaceModifierInstanceForm	= 0x0102D12C,
	kVtbl_ImageSpaceModifierInstance		= 0x0102D1CC,
	kVtbl_ImageSpaceModifierInstanceDOF		= 0x0102D2A4,
	kVtbl_ImageSpaceModifierInstanceRB		= 0x0102D36C,
	kVtbl_ImageSpaceModifierInstanceDRB		= 0x0102D454,
	kVtbl_ImageSpaceModifierInstanceTemp	= 0x0102D50C,

	// lit object shader properties
	kVtbl_BSShaderPPLightingProperty		= 0x010AE0D0,
	kVtbl_Lighting30ShaderProperty			= 0x010B9910,
};
