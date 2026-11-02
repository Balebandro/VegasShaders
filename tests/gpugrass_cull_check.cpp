// Offline GPU check of GpuGrassCull.spv (NVR GPU grass): runs the real cull shader on the real GPU with plain Vulkan, fed the way
// effects/GpuGrass.cpp feeds it (frame block built from a game-style camera with the camera folded into the rows, one draw record
// per grass draw, tufts packed as the game packs them), and checks the counts. A test tool, not part of NVR (needs no game, no DXVK).
// Build (x64 Native Tools prompt): cl /O2 /EHsc /I %VULKAN_SDK%\Include gpugrass_cull_check.cpp /link %VULKAN_SDK%\Lib\vulkan-1.lib
// Run: gpugrass_cull_check.exe <Compute\GpuGrassCull.spv> [density] [clash]   exit 0 = every tuft accounted for and strands written.
// "clash" binds the tuft buffer where the frame block belongs: the 2026-10-27 bulk-run bug (uniform slot 0 = storage slot 0 in the
// extension); it prints exactly the game's log of that run: 0 clumps drawn, 0 outside the view, 1 past the fade start.
#include <vulkan/vulkan.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <random>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAILED %s -> %d (line %d)\n", #x, (int)r_, __LINE__); return 1; } } while (0)

struct DrawRec { float shape[4], scaleMask[4], sun[4], ambient[4], ramp[4][4]; };
struct FrameBlock {
	float rows[4][4];
	float cam[4], sunDir[4], wind[4], fade[4], fogParam[4], fogColor[4], lod[4], shape[4], shape2[4], shape3[4], lanes[4], misc[4], look[4];
	float planes[4][4];
};
static_assert(sizeof(FrameBlock) == 336, "layout");
static_assert(sizeof(DrawRec) == 128, "layout");

struct Buf { VkBuffer b = VK_NULL_HANDLE; VkDeviceMemory m = VK_NULL_HANDLE; void* p = nullptr; VkDeviceSize size = 0; };

static VkDevice gDev; static VkPhysicalDevice gPhys;

static bool MakeBuf(Buf& out, VkDeviceSize size, VkBufferUsageFlags usage) {
	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(gDev, &bi, nullptr, &out.b) != VK_SUCCESS) return false;
	VkMemoryRequirements req; vkGetBufferMemoryRequirements(gDev, out.b, &req);
	VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(gPhys, &mp);
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { type = i; break; }
	if (type == UINT32_MAX) return false;
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = req.size; ai.memoryTypeIndex = type;
	if (vkAllocateMemory(gDev, &ai, nullptr, &out.m) != VK_SUCCESS) return false;
	vkBindBufferMemory(gDev, out.b, out.m, 0);
	vkMapMemory(gDev, out.m, 0, VK_WHOLE_SIZE, 0, &out.p);
	memset(out.p, 0, (size_t)size);
	out.size = size;
	return true;
}

static float Frac01(float v) { return v - floorf(v); }

int main(int argc, char** argv) {
	const char* spvPath = argc > 1 ? argv[1] : "GpuGrassCull.spv";
	const float density = argc > 2 ? (float)atof(argv[2]) : 1.0f;
	const bool clash = argc > 3 && !strcmp(argv[3], "clash");   // the bulk run's bug: the frame block's binding gets the tuft buffer
	FILE* fp = fopen(spvPath, "rb");
	if (!fp) { printf("no %s\n", spvPath); return 1; }
	std::vector<uint32_t> code;
	{ fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET); code.resize(n / 4); fread(code.data(), 1, n, fp); fclose(fp); }

	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO }; app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ici.pApplicationInfo = &app;
	VkInstance inst; CHECK(vkCreateInstance(&ici, nullptr, &inst));
	uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, nullptr);
	std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(inst, &n, pds.data());
	if (!n) { printf("no GPU\n"); return 1; }
	gPhys = pds[0];
	for (auto pd : pds) { VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp); if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { gPhys = pd; break; } }
	VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(gPhys, &props);
	printf("GPU: %s\n", props.deviceName);
	uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(gPhys, &qn, nullptr);
	std::vector<VkQueueFamilyProperties> qf(qn); vkGetPhysicalDeviceQueueFamilyProperties(gPhys, &qn, qf.data());
	uint32_t family = 0; for (uint32_t i = 0; i < qn; i++) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO }; qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO }; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
	CHECK(vkCreateDevice(gPhys, &dci, nullptr, &gDev));
	VkQueue queue; vkGetDeviceQueue(gDev, family, 0, &queue);

	// --- a game-style camera: D3D row-vector view x reversed-depth perspective; the shader's rows = the transposed matrix (c9-c12)
	const double eye[3] = { -70212.37, 1534.81, 8122.40 };
	const double yaw = 35.0 * 3.14159265358979 / 180.0, pitch = -8.0 * 3.14159265358979 / 180.0;
	const double fwd[3] = { sin(yaw) * cos(pitch), cos(yaw) * cos(pitch), sin(pitch) };
	double right[3] = { fwd[1], -fwd[0], 0.0 }; { double l = sqrt(right[0] * right[0] + right[1] * right[1]); right[0] /= l; right[1] /= l; }
	const double up[3] = { right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2], right[0] * fwd[1] - right[1] * fwd[0] };
	const double p11 = 1.0 / tan(45.0 * 3.14159265358979 / 180.0), p22 = p11 * 16.0 / 9.0, zn = 5.0, zf = 300000.0;
	const double A = zn / (zn - zf), B = -zf * zn / (zn - zf);
	const double* axes[3] = { right, up, fwd };
	float game[4][4];   // what the game's grass VS has in c9-c12 (float)
	for (int i = 0; i < 2; i++) { const double s = i == 0 ? p11 : p22; for (int k = 0; k < 3; k++) game[i][k] = (float)(axes[i][k] * s); game[i][3] = (float)(-s * (axes[i][0] * eye[0] + axes[i][1] * eye[1] + axes[i][2] * eye[2])); }
	for (int k = 0; k < 3; k++) { game[2][k] = (float)(fwd[k] * A); game[3][k] = (float)fwd[k]; }
	const double fe = fwd[0] * eye[0] + fwd[1] * eye[1] + fwd[2] * eye[2];
	game[2][3] = (float)(-fe * A + B); game[3][3] = (float)(-fe);

	// --- the frame block exactly as GpuGrass.cpp RenderStrands builds it
	// EyeFromRows (rows 0, 1, 3)
	double a[3][3] = { { game[0][0], game[0][1], game[0][2] }, { game[1][0], game[1][1], game[1][2] }, { game[3][0], game[3][1], game[3][2] } };
	double bb[3] = { -game[0][3], -game[1][3], -game[3][3] };
	auto det3 = [](double m[3][3]) { return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]); };
	const double det = det3(a); double e[3];
	for (int k = 0; k < 3; k++) { double m[3][3]; for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) m[r][c] = c == k ? bb[r] : a[r][c]; e[k] = det3(m) / det; }
	FrameBlock f = {};
	for (int i = 0; i < 4; i++) { for (int k = 0; k < 3; k++) f.rows[i][k] = game[i][k]; f.rows[i][3] = (float)((double)game[i][3] + game[i][0] * e[0] + game[i][1] * e[1] + game[i][2] * e[2]); }
	const unsigned drawsN = 18, perDraw = 228;
	const unsigned tuftsN = drawsN * perDraw;
	f.cam[0] = (float)e[0]; f.cam[1] = (float)e[1]; f.cam[2] = (float)e[2]; f.cam[3] = (float)tuftsN;
	f.sunDir[0] = 0.3f; f.sunDir[1] = 0.2f; f.sunDir[2] = 0.93f; f.sunDir[3] = 1.5f;
	f.wind[0] = 0.0f; f.wind[1] = 1.0f; f.wind[2] = 20.0f; f.wind[3] = 1.3f;
	f.fade[0] = 7000.0f; f.fade[1] = 2500.0f; f.fade[2] = density; f.fade[3] = 60.0f;
	f.fogParam[0] = 60000.0f; f.fogParam[1] = 50000.0f; f.fogParam[2] = 1.0f;
	f.lod[0] = 600.0f; f.lod[1] = 1.0f / 1900.0f; f.lod[2] = 64.0f; f.lod[3] = 8.0f;
	f.shape[0] = 0.9f; f.shape[1] = 1.0f; f.shape[2] = 0.4f; f.shape[3] = 0.15f;
	f.shape2[0] = 1.0f; f.shape2[1] = 0.5f; f.shape2[2] = 0.5f; f.shape2[3] = 0.5f;
	const float rp11 = sqrtf(f.rows[0][0] * f.rows[0][0] + f.rows[0][1] * f.rows[0][1] + f.rows[0][2] * f.rows[0][2]);
	f.shape3[0] = 0.15f; f.shape3[1] = 0.75f; f.shape3[2] = 2.0f / (rp11 * 1920.0f); f.shape3[3] = 1200.0f;
	const unsigned cap = 524288;
	f.misc[1] = (float)cap;
	f.look[0] = 0.2f; f.look[1] = 0.3f; f.look[2] = 1.0f;
	for (int p = 0; p < 4; p++) {
		const int axis = p < 2 ? 0 : 1; const float sign = (p & 1) ? -1.0f : 1.0f; float nn[4];
		for (int k = 0; k < 4; k++) nn[k] = f.rows[3][k] + sign * f.rows[axis][k];
		const float len = sqrtf(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
		for (int k = 0; k < 4; k++) f.planes[p][k] = nn[k] / len;
	}

	// --- draws and tufts: 18 draws of 228 tufts each round the camera (the game's packing: frac(xyz) = slope normal, w = size + light)
	std::vector<DrawRec> draws(drawsN);
	std::vector<float> tufts(tuftsN * 4);
	std::vector<uint32_t> tuftDraw(tuftsN);
	std::mt19937 rng(7);
	std::uniform_real_distribution<float> U(0.0f, 1.0f);
	for (unsigned d = 0; d < drawsN; d++) {
		DrawRec& r = draws[d];
		r.shape[0] = 48.0f; r.shape[1] = 63.5f; r.shape[2] = 0.02f; r.shape[3] = 0.004f;
		r.scaleMask[0] = r.scaleMask[1] = r.scaleMask[2] = 1.0f; r.scaleMask[3] = (d & 1) ? 0.0f : 1.0f;   // even draws (draw 0 too, as in the bulk run): part 1 (mixed)
		r.sun[0] = 1.0f; r.sun[1] = 0.9f; r.sun[2] = 0.8f; r.sun[3] = 1.0f;
		r.ambient[0] = 0.3f; r.ambient[1] = 0.3f; r.ambient[2] = 0.35f; r.ambient[3] = 0.3f * 2.0f * 63.5f * 48.0f;
		for (int b = 0; b < 4; b++) { r.ramp[b][0] = 0.7f; r.ramp[b][1] = 0.58f; r.ramp[b][2] = 0.38f; r.ramp[b][3] = 0.6f + 0.1f * b; }
		// a piece of land: a 2048-unit square somewhere within 6000 units of the camera
		float cx = (float)e[0] + (U(rng) * 2.0f - 1.0f) * 6000.0f, cy = (float)e[1] + (U(rng) * 2.0f - 1.0f) * 6000.0f;
		if (d == 1) { cx = (float)(e[0] + fwd[0] * 900.0); cy = (float)(e[1] + fwd[1] * 900.0); }   // one piece right in front of the camera (near strands)
		for (unsigned i = 0; i < perDraw; i++) {
			const unsigned t = d * perDraw + i;
			const float x = floorf(cx + (U(rng) - 0.5f) * 2048.0f), y = floorf(cy + (U(rng) - 0.5f) * 2048.0f), z = floorf((float)e[2] - 150.0f);
			const float nx = 0.05f * (U(rng) - 0.5f), ny = 0.05f * (U(rng) - 0.5f), nz = 0.99f;
			tufts[t * 4 + 0] = x + (nx * 0.5f + 0.5f); tufts[t * 4 + 1] = y + (ny * 0.5f + 0.5f); tufts[t * 4 + 2] = z + (nz * 0.5f + 0.5f);
			tufts[t * 4 + 3] = floorf(U(rng) * 40.0f) + U(rng) * 0.99f;
			tuftDraw[t] = d;
		}
	}
	// what the CPU expects: tufts on screen (centre test, a margin like the shader's) and within the fade start for part-1 draws
	unsigned expectPast = 0;
	for (unsigned t = 0; t < tuftsN; t++) {
		const float* q = &tufts[t * 4];
		double rel[3] = { q[0] - e[0], q[1] - e[1], q[2] - e[2] };
		double c[4]; for (int i = 0; i < 4; i++) c[i] = f.rows[i][0] * rel[0] + f.rows[i][1] * rel[1] + f.rows[i][2] * rel[2] + f.rows[i][3];
		if (!(tuftDraw[t] & 1) && sqrt(c[0] * c[0] + c[1] * c[1] + c[3] * c[3]) > 7000.0) expectPast++;
	}

	Buf frame, tb, tdb, db, blades, args;
	if (!MakeBuf(frame, sizeof(f), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !MakeBuf(tb, tufts.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
		!MakeBuf(tdb, tuftDraw.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !MakeBuf(db, draws.size() * sizeof(DrawRec), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
		!MakeBuf(blades, (VkDeviceSize)cap * 2 * 48, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !MakeBuf(args, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) { printf("buffers failed\n"); return 1; }
	memcpy(frame.p, &f, sizeof(f)); memcpy(tb.p, tufts.data(), tufts.size() * 4); memcpy(tdb.p, tuftDraw.data(), tuftDraw.size() * 4);
	memcpy(db.p, draws.data(), draws.size() * sizeof(DrawRec));
	const uint32_t init[16] = { 15, 0, 0, 0, 3, 0, 0, 0 }; memcpy(args.p, init, 64);

	VkDescriptorSetLayoutBinding lb[6] = {};
	for (int i = 0; i < 6; i++) { lb[i].binding = i; lb[i].descriptorCount = 1; lb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; lb[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; }
	VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO }; lci.bindingCount = 6; lci.pBindings = lb;
	VkDescriptorSetLayout dsl; CHECK(vkCreateDescriptorSetLayout(gDev, &lci, nullptr, &dsl));
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO }; plci.setLayoutCount = 1; plci.pSetLayouts = &dsl;
	VkPipelineLayout pl; CHECK(vkCreatePipelineLayout(gDev, &plci, nullptr, &pl));
	VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO }; smci.codeSize = code.size() * 4; smci.pCode = code.data();
	VkShaderModule sm; CHECK(vkCreateShaderModule(gDev, &smci, nullptr, &sm));
	VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = sm; cpci.stage.pName = "main"; cpci.layout = pl;
	VkPipeline pipe; CHECK(vkCreateComputePipelines(gDev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe));
	VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 } };
	VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO }; dpci.maxSets = 1; dpci.poolSizeCount = 2; dpci.pPoolSizes = ps;
	VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(gDev, &dpci, nullptr, &pool));
	VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO }; dsai.descriptorPool = pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &dsl;
	VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(gDev, &dsai, &set));
	Buf* bufs[6] = { &frame, &tb, &tdb, &db, &blades, &args };
	VkDescriptorBufferInfo dbi[6]; VkWriteDescriptorSet w[6] = {};
	for (int i = 0; i < 6; i++) {
		dbi[i] = { bufs[i]->b, 0, VK_WHOLE_SIZE };
		if (i == 0 && clash) dbi[i] = { tb.b, 0, sizeof(FrameBlock) };
		w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = set; w[i].dstBinding = i; w[i].descriptorCount = 1;
		w[i].descriptorType = lb[i].descriptorType; w[i].pBufferInfo = &dbi[i];
	}
	vkUpdateDescriptorSets(gDev, 6, w, 0, nullptr);
	VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; cpi.queueFamilyIndex = family;
	VkCommandPool cp; CHECK(vkCreateCommandPool(gDev, &cpi, nullptr, &cp));
	VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO }; cbai.commandPool = cp; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
	VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(gDev, &cbai, &cb));
	VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	vkBeginCommandBuffer(cb, &cbbi);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, nullptr);
	const unsigned clumps = (unsigned)ceilf(density < 1.0f ? 1.0f : density);
	vkCmdDispatch(cb, (tuftsN * clumps + 63) / 64, 1, 1);
	VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER }; mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
	vkEndCommandBuffer(cb);
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
	CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
	CHECK(vkQueueWaitIdle(queue));

	const uint32_t* c = (const uint32_t*)args.p;
	if (clash) printf("(CLASH: the frame block's binding reads the tuft buffer, as uniform slot 0 = storage slot 0 did in the extension)\n");
	printf("density %.2f: %u tufts uploaded | GPU: %u clumps drawn, %u outside the view, %u past the fade start (CPU expects %u); strands near %u, far %u, written %u, did not fit %u; args near {%u %u} far {%u %u}\n",
		density, tuftsN, c[8], c[9], c[12], expectPast, c[1], c[5], c[10], c[11], c[0], c[1], c[4], c[5]);
	const unsigned seen = c[8] + c[9] + c[12];
	printf("every tuft accounted for: %s (%u of %u)\n", seen >= tuftsN ? "YES" : "NO", seen, tuftsN);
	// a look at some strands
	const float* bl = (const float*)blades.p;
	double minW = 1e9, maxW = 0, minH = 1e9, maxH = 0; unsigned bad = 0;
	for (unsigned i = 0; i < c[1] && i < cap; i++) {
		const float* s = bl + i * 12;
		for (int k = 0; k < 12; k++) if (k != 7 && k != 11 && !std::isfinite(s[k])) bad++;
		minW = fmin(minW, s[3]); maxW = fmax(maxW, s[3]); minH = fmin(minH, s[6]); maxH = fmax(maxH, s[6]);
	}
	printf("near strands: width %.2f-%.2f units, tip height %.1f-%.1f units, non-finite values %u\n", minW, maxW, minH, maxH, bad);
	minW = 1e9; maxW = 0;
	for (unsigned i = 0; i < c[5] && i < cap; i++) { const float* s = bl + (cap + i) * 12; minW = fmin(minW, s[3]); maxW = fmax(maxW, s[3]); }
	printf("far strands: width %.2f-%.2f units\n", minW, maxW);
	return seen >= tuftsN && (c[1] + c[5]) > 0 ? 0 : 2;
}
