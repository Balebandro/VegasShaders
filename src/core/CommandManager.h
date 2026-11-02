#pragma once

// Owns the script command table. The plugin instance lives for the whole session.
class CommandManager : public CommandManagerBase {
public:
	// Command bodies. Each one writes its outcome to *out (1 = done, 0 = not done) or to a value.
	class PluginCommands {
	public:
		void IsThirdPerson(double* out);
		void GetVersion(double* out);
		void GetLocationName(double* out);
		void SetExtraEffectEnabled(double* out, const char* effect, bool enabled);
		void SetCustomConstant(double* out, const char* name, D3DXVECTOR4* vec);
		void GetWeatherName(double* out);
		void GetSetting(double* out, const char* section, const char* key);
		void SetSetting(double* out, const char* section, const char* key, float value);
		void SetSettingS(double* out, const char* section, const char* key, const char* text);
		void CameraTranslate(double* out, TESObjectREFR* target, NiPoint3* offset);
		void CameraRotate(double* out, TESObjectREFR* target, NiPoint3* angles);
		void CameraTranslateToPosition(double* out, NiPoint3* point);
		void CameraRotateToPosition(double* out, NiPoint3* angles);
		void CameraLookAt(double* out, TESObjectREFR* target, NiPoint3* offset);
		void CameraLookAtPosition(double* out, NiPoint3* point);
		void CameraReset(double* out);
	};

	static void Initialize(const PluginInterface* iface);

	PluginCommands Commands;
};
