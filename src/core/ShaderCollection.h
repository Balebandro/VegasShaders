#pragma once

#include "ShaderTemplate.h"	// std::map/std::string come with Framework.h

// A family of the game's own shaders (PBR, POM, Skin, Grass...) that NVR replaces as a group:
// one menu switch, one template table, and the records of every shader in it.
class ShaderCollection
{
public:
	ShaderCollection(const char* CollectionName);
	virtual ~ShaderCollection() {}

	// Per-family hooks the subclasses fill in; the base does nothing.
	virtual void	UpdateConstants() {}
	virtual void	UpdateSettings() {}
	virtual void	RegisterConstants() {}
	virtual void	RegisterTextures() {}

	bool			SwitchShader();		// flips Enabled, stores it, applies it to every record
	void			DisposeShaders();	// releases the records of every shader in both lists
	bool			IsLoaded();			// (declared only)
	bool			Enabled;			// the family's menu switch ([Shaders] status)

	const char*							Name;	// family name, also its settings key
	std::vector<NiD3DPixelShaderEx*>	PixelShaderList;	// the family's game shaders, filled as they load
	std::vector<NiD3DVertexShaderEx*>	VertexShaderList;

	// Shader file name -> template it is compiled from; empty when the family has none.
	virtual std::map<std::string_view, ShaderTemplate> Templates() { return {}; }
	ShaderTemplate	GetTemplate(const char* ShaderName);
};
