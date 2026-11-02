ShaderCollection::ShaderCollection(const char* CollectionName) {
	Logger::Log("ShaderCollection init: %s", CollectionName);
	Name = CollectionName;
	Enabled = TheSettingManager->GetMenuShaderEnabled(CollectionName);
}

// Sets every loaded shader of the family to the same on/off state.
template <typename List> static void ApplyEnabled(List& Shaders, bool On) {
	for (auto* Shader : Shaders) Shader->Enabled = On;
}

bool ShaderCollection::SwitchShader() {
	const bool turnOn = !TheSettingManager->GetMenuShaderEnabled(Name);
	Enabled = turnOn;
	TheSettingManager->SetMenuShaderEnabled(Name, turnOn);

	// The records stay loaded; only their switch changes.
	ApplyEnabled(PixelShaderList, turnOn);
	ApplyEnabled(VertexShaderList, turnOn);
	return turnOn;
}

void ShaderCollection::DisposeShaders() {
	for (NiD3DPixelShaderEx* Shader : PixelShaderList) Shader->DisposeShader();
	for (NiD3DVertexShaderEx* Shader : VertexShaderList) Shader->DisposeShader();
}

ShaderTemplate ShaderCollection::GetTemplate(const char* ShaderName) {
	const auto table = Templates();
	const auto found = table.find(ShaderName);
	return found == table.end() ? ShaderTemplate{} : found->second;
}
