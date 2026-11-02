#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include "SettingManager.h" // toml11 setup and the tomlValue type

// Location presets. A preset is a TOML snapshot of the settings kept in Data\VegasShaders\Presets; Variants are partial overlays in
// Data\VegasShaders\Variants (an older install's Data\NVR\ is used while the new folder is missing, Paths.h). Everything is
// static: the keyword cell tags, preset/variant file I/O, the blacklist, choosing the preset for the current place, and pushing it
// into SettingManager. The state behind it lives in PresetManager.cpp.
// Design notes: docs/preset-manager-design.md.
class PresetManager
{
public:
	// A stored value. Bools and ints travel as floats; SettingManager::SetSettingF converts to the live setting's own type.
	struct PresetValue {	// one stored value
		enum class ValueType { Float, String };
		ValueType	Type;
		float		FloatValue;		// Float entries (bool, int or float settings)
		std::string	StringValue;	// String entries
	};
	using PresetSection	= std::unordered_map<std::string, PresetValue>;		// key name -> value
	using PresetData	= std::unordered_map<std::string, PresetSection>;	// dotted section -> its keys

	// Folders and file names under the game directory (part of the on-disk format).
	// Under Paths::DataRoot(): Data\VegasShaders\ (or the old Data\NVR\), relative to the game folder
	static std::string kKeywordsDir()			{ return std::string(Paths::DataRoot()) + "Keywords\\"; }
	static std::string kPresetsDir()			{ return std::string(Paths::DataRoot()) + "Presets\\"; }
	static std::string kVariantsDir()			{ return std::string(Paths::DataRoot()) + "Variants\\"; }
	static std::string kEnabledVariantsPath()	{ return std::string(Paths::DataRoot()) + "EnabledVariants.ini"; }
	static constexpr auto kDefaultInteriorName	= "DefaultInterior";
	static constexpr auto kDefaultExteriorName	= "DefaultExterior";

	static void Initialize();	// reads the keyword files and the enabled Variant list (once, at boot)

	// Keyword tags (interior cells only; worldspaces are never tagged).
	static const std::string*		GetKeywordForCell(const std::string& editorId);
	static UInt32					CountCellsForKeyword(const std::string& keyword);
	static std::vector<std::string>	GetKnownKeywordCells();	// every tagged cell, sorted (the dev tools' coc picker lists them)

	// Preset files: the name is bare (no folder, no extension), e.g. "DefaultInterior" or an EditorID. Always read from disk.
	static bool ReadPreset(const std::string& name, PresetData& into);
	static bool WritePreset(const std::string& name, const PresetData& contents);
	static bool PresetExists(const std::string& name);
	// Permanent, no undo: callers confirm first. The two default names are refused; a missing file or a filesystem error
	// also gives false.
	static bool DeletePreset(const std::string& name);

	// Variant files: same format, own folder.
	static bool ReadVariant(const std::string& name, PresetData& into);
	static bool WriteVariant(const std::string& name, const PresetData& contents);
	static bool VariantExists(const std::string& name);

	// Sections (full dotted path, e.g. "Main.CameraMode.Main") that never go into or come out of a preset.
	static bool IsBlacklistedSection(const std::string& dottedSection);
	// DefaultInterior / DefaultExterior: no keyword may use them.
	static bool IsReservedName(const std::string& name);

	// ---- choosing and applying ------------------------------------------------

	enum class ResolvedTier { Default, Keyword, Override };	// the rule that picked the preset

	// The last resolve, for the debug window.
	struct ResolveResult {	// what the last resolve found
		ResolvedTier	Tier = ResolvedTier::Default;	// which rule matched
		std::string		PresetName;					// file used; empty when the shipped defaults were used
		bool			UsedRawDefaults = false;	// no DefaultInterior / DefaultExterior file existed
		std::string		Keyword;					// the cell's keyword, if any (informational)
		std::string		CellEditorID;				// the cell's EditorID
		std::string		WorldspaceEditorID;			// outdoors: the worldspace's EditorID
		bool			IsInterior = false;			// the cell is an interior
	};

	// Picks and applies the preset for this cell. Automatic is the per-location call (ShaderManager::UpdateConstants, on a
	// real place change); without it the preset is always re-read and re-applied (the menu's Save, Reload and Delete).
	// UNOFFICIAL 2026-10-19: an automatic switch to the preset that is already active changes nothing, so live menu edits
	// survive a save reload or a walk between two places sharing a preset. A place without an authored preset gets the
	// player's own settings, as they were when the player last left such a place, not the shipped defaults.
	static void					ResolveAndApply(TESObjectCELL* Cell, bool Automatic = false);
	// UNOFFICIAL 2026-10-19: drops the active identity and the saved own settings (the master switch, when turned on).
	static void					ForgetSession();
	static const ResolveResult&	GetLastResolveResult();	// for the debug window

	// No file I/O. True when the place identity changed since the last true: indoors the cell, outdoors the worldspace, and
	// any interior/exterior crossing. Each true moves the stored identity, so one caller, once per frame.
	static bool					ShouldReResolve(TESObjectCELL* cell);

	// ---- authoring ------------------------------------------------------------

	// Every non-blacklisted setting's current value (a full snapshot, not a diff): the keys and types come from the shipped
	// defaults, the values from the live getters. This is what Save writes.
	static bool			CaptureLiveState(PresetData& into);

	// ---- preset browser -------------------------------------------------------

	enum class PresetKind { Keyword, Default, Override };	// ordinal = sort order in the browser
	struct PresetListEntry {	// one row of the browser
		std::string	Name;	// bare preset name
		PresetKind	Kind;	// decided from the name alone
	};

	static std::vector<PresetListEntry>	ListAllPresets();	// the folder, read on every call; by kind, then name

	// Pushes a preset in like a real resolve, but leaves the last result and the generation alone (a preview; the next real
	// resolve puts the place's own preset back).
	static void		ApplyPreviewPreset(const PresetData& contents);

	static UInt32	GetResolveGeneration();	// bumped by every real resolve: a preview seeing it change drops itself

	// ---- variants -------------------------------------------------------------

	static const std::vector<std::string>&	GetEnabledVariants();	// lowest priority first (later ones win)
	// Enabling appends (top priority), disabling removes; EnabledVariants.ini is rewritten at once.
	static void								SetVariantEnabled(const std::string& name, bool enable);
	static std::vector<std::string>			ListAllVariants();	// every Variant file, enabled or not, by name

	// The live values that differ from what the last preset apply pushed (a key the baseline lacks counts as different).
	static void		CaptureVariantDiff(PresetData& into);

	// One preset file's changes from a refresh run (the caller logs them).
	struct RefreshSummary {	// one changed file
		std::string					PresetName;	// the file
		std::vector<std::string>	AddedKeys;	// each "Section.Key = value", already formatted
	};

	// Adds to every preset file each eligible default key it lacks, at its default value. Nothing existing is changed or
	// removed. Only files that changed are listed.
	static std::vector<RefreshSummary>	RefreshAllPresets();	// one summary per file written
};
