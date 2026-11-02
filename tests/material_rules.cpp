// Offline check of the material rules (src/core/MaterialRules.h, resource/VegasShaders.materials.txt).
//   material_rules.exe <materials.txt> [textures.txt]
// Always: the table loads without errors and the known cases below land where they should (exit 1 if not).
// With a texture list (tools/material-rules/list_textures.py output): class counts over the colour textures, plus up to
// 20 evenly spread members of each class, to look for wrong classifications by eye.
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

#include "../src/core/MaterialRules.h"

static int failures = 0;

static void Expect(const MaterialRules::Table& table, const char* path, const char* expected) {
	const std::string got = table.Get(table.Match(MaterialRules::Normalize(path))).Name;
	if (got != expected) {
		printf("FAIL %s -> %s (expected %s)\n", path, got.c_str(), expected);
		++failures;
	}
}

static bool IsColourTexture(const std::string& p) {
	static const char* const suffixes[] = { "_n.dds", "_m.dds", "_g.dds", "_em.dds", "_s.dds", "_h.dds", "_e.dds", "_p.dds", "_sk.dds", "_hl.dds" };
	for (const char* s : suffixes) {
		const size_t n = strlen(s);
		if (p.size() >= n && p.compare(p.size() - n, n, s) == 0) return false;
	}
	return p.find("\\interface\\") == std::string::npos && p.find("\\lod\\") == std::string::npos;
}

int main(int argc, char** argv) {
	if (argc < 2) { printf("usage: material_rules <materials.txt> [textures.txt]\n"); return 2; }
	std::ifstream in(argv[1], std::ios::binary);
	if (!in) { printf("cannot read %s\n", argv[1]); return 2; }
	std::stringstream text;
	text << in.rdbuf();
	MaterialRules::Table table;
	std::string errors;
	if (!table.Load(text.str(), errors)) { printf("TABLE ERRORS:\n%s", errors.c_str()); ++failures; }

	// Normalisation.
	if (MaterialRules::Normalize("Data\\Textures\\Weapons\\Foo.DDS") != "textures\\weapons\\foo.dds") { printf("FAIL normalise 1\n"); ++failures; }
	if (MaterialRules::Normalize("weapons/foo.dds") != "textures\\weapons\\foo.dds") { printf("FAIL normalise 2\n"); ++failures; }
	if (MaterialRules::Normalize("C:\\Games\\FNV\\Data\\textures\\a.dds") != "textures\\a.dds") { printf("FAIL normalise 3\n"); ++failures; }

	// Known cases (the plan's false positives among them).
	Expect(table, "textures\\weapons\\2handrifle\\huntingrifle.dds", "gun");
	Expect(table, "textures\\weapons\\2handlauncher\\1stpersonrockitlauncher.dds", "gun");
	Expect(table, "textures\\weapons\\1handmelee\\machete.dds", "blade");
	Expect(table, "textures\\projectiles\\projectilebullet.dds", "default");
	Expect(table, "textures\\characters\\hair\\hairgreasy.dds", "default");
	Expect(table, "textures\\characters\\male\\upperbodymale.dds", "default");
	Expect(table, "textures\\interface\\icons\\metal.dds", "default");
	Expect(table, "textures\\landscape\\plants\\nvgoldfingerbananafrond.dds", "plant");
	Expect(table, "textures\\clutter\\neon\\neonironsights.dds", "sign");
	Expect(table, "textures\\architecture\\strip\\stripchrome01.dds", "chrome");
	Expect(table, "textures\\architecture\\barn\\barnwood01.dds", "wood");
	Expect(table, "textures\\architecture\\whatever\\wall01.dds", "building");
	Expect(table, "textures\\dungeons\\vault\\vaultwall01.dds", "interiormetal");
	Expect(table, "textures\\armor\\powerarmor\\t45d.dds", "armourmetal");
	Expect(table, "textures\\armor\\leathergear\\leatherjacket.dds", "leather");
	Expect(table, "textures\\nvdlc03\\weapons\\x\\gun.dds", "gun");
	Expect(table, "textures\\dlcanch\\clutter\\box.dds", "clutter");
	Expect(table, "textures\\nvdlc01\\interface\\metal.dds", "default");
	Expect(table, "textures\\clutter\\glassesblackrimmed.dds", "glass");
	Expect(table, "textures\\effects\\fxglassspray01.dds", "default");
	Expect(table, "textures\\landscape\\lod\\anything.dds", "default");
	Expect(table, "textures\\somemod\\unknownthing.dds", "default");

	if (argc >= 3) {
		std::ifstream list(argv[2]);
		std::map<std::string, std::vector<std::string>> byClass;
		std::string line;
		size_t total = 0;
		while (std::getline(list, line)) {
			if (!line.empty() && line.back() == '\r') line.pop_back();
			const std::string p = MaterialRules::Normalize(line.c_str());
			if (!IsColourTexture(p)) continue;
			++total;
			byClass[table.Get(table.Match(p)).Name].push_back(p);
		}
		printf("%zu colour textures\n", total);
		for (auto& c : byClass) printf("  %-14s %6zu\n", c.first.c_str(), c.second.size());
		for (auto& c : byClass) {
			if (c.first == "default") continue;
			printf("\n[%s]\n", c.first.c_str());
			const size_t n = c.second.size(), shown = n < 20 ? n : 20;
			for (size_t i = 0; i < shown; ++i) printf("  %s\n", c.second[i * n / shown].c_str());
		}
	}
	printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
	return failures ? 1 : 0;
}
