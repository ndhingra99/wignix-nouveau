/* Offscreen Vulkan test, step 7: storage images of many formats and kinds.
 * For each format and for 2D, 2D-array and 3D images (20x12, 3 layers or
 * slices) a compute shader stores a pattern and another loads it back into
 * a buffer as four 32-bit words per texel; the words must be the pattern
 * (components the format lacks read as 0, 0, 0, 1).  RGBA8 images are also
 * copied to a buffer and compared byte by byte.
 * usage: vksi [format]    (shaders in ./si, made by gen_si.py)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define W 20
#define H 12
#define D 3
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s failed: %d\n", #x, r_); exit(1); } } while (0)

static VkPhysicalDevice pdev;
static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pdev, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	exit(1);
}

static uint8_t *make_buffer(VkBuffer *buf, VkDeviceSize size)
{
	VkMemoryRequirements mr;
	VkDeviceMemory mem;
	uint8_t *map;
	CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo){
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT },
		NULL, buf));
	vkGetBufferMemoryRequirements(dev, *buf, &mr);
	CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) },
		NULL, &mem));
	CHECK(vkBindBufferMemory(dev, *buf, mem, 0));
	CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&map));
	memset(map, 0x55, size);
	return map;
}

static VkCommandBuffer begin(void)
{
	VkCommandBuffer cmd;
	CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 }, &cmd));
	CHECK(vkBeginCommandBuffer(cmd, &(VkCommandBufferBeginInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }));
	return cmd;
}

static void run(VkCommandBuffer cmd)
{
	CHECK(vkEndCommandBuffer(cmd));
	CHECK(vkQueueSubmit(queue, 1, &(VkSubmitInfo){
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1, .pCommandBuffers = &cmd }, VK_NULL_HANDLE));
	CHECK(vkQueueWaitIdle(queue));
}

static void layout(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
		   VkImageLayout from, VkImageLayout to)
{
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1,
		&(VkImageMemoryBarrier){
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = 0, .dstAccessMask = 0x7ff,
			.oldLayout = from, .newLayout = to,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = img, .subresourceRange = { aspect, 0, 1, 0, 1 } });
}

static VkPipelineLayout pl;
static VkDescriptorSetLayout dsl;

static VkPipeline make_pipe(const char *file, VkDescriptorType type0)
{
	static uint32_t code[16384];
	VkShaderModule mod;
	VkPipeline pipe;
	FILE *f = fopen(file, "rb");
	size_t size;
	if (!f)
		exit(1);
	size = fread(code, 1, sizeof(code), f);
	fclose(f);
	CHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = size, .pCode = code }, NULL, &mod));
	CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2,
		.pBindings = (VkDescriptorSetLayoutBinding[]){
			{ .binding = 0, .descriptorType = type0,
			  .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
			{ .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			  .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT } } },
		NULL, &dsl));
	CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &dsl }, NULL, &pl));
	CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main" },
		.layout = pl }, NULL, &pipe));
	return pipe;
}


static const struct {
	const char *name;
	VkFormat format;
	char kind;
	int comps;
} formats[] = {
	{ "rgba8", VK_FORMAT_R8G8B8A8_UNORM, 'n', 4 },
	{ "rgba8_snorm", VK_FORMAT_R8G8B8A8_SNORM, 'n', 4 },
	{ "rgba8ui", VK_FORMAT_R8G8B8A8_UINT, 'u', 4 },
	{ "rgba8i", VK_FORMAT_R8G8B8A8_SINT, 'i', 4 },
	{ "rgba16ui", VK_FORMAT_R16G16B16A16_UINT, 'u', 4 },
	{ "rgba16i", VK_FORMAT_R16G16B16A16_SINT, 'i', 4 },
	{ "rgba16f", VK_FORMAT_R16G16B16A16_SFLOAT, 'f', 4 },
	{ "r32ui", VK_FORMAT_R32_UINT, 'u', 1 },
	{ "r32i", VK_FORMAT_R32_SINT, 'i', 1 },
	{ "r32f", VK_FORMAT_R32_SFLOAT, 'f', 1 },
	{ "rgba32ui", VK_FORMAT_R32G32B32A32_UINT, 'u', 4 },
	{ "rgba32i", VK_FORMAT_R32G32B32A32_SINT, 'i', 4 },
	{ "rgba32f", VK_FORMAT_R32G32B32A32_SFLOAT, 'f', 4 },
	{ "rg16f", VK_FORMAT_R16G16_SFLOAT, 'f', 2 },
	{ "r8", VK_FORMAT_R8_UNORM, 'n', 1 },
	{ "rgb10_a2", VK_FORMAT_A2B10G10R10_UNORM_PACK32, 'n', 4 },
	{ "r11f_g11f_b10f", VK_FORMAT_B10G11R11_UFLOAT_PACK32, 'f', 3 },
};
static const char *dims[3] = { "2d", "2da", "3d" };

static void expect(char kind, int comps, int x, int y, int z, uint32_t out[4])
{
	float f[4];
	int32_t i[4];
	int c;
	if (kind == 'f') {
		f[0] = x; f[1] = y; f[2] = x + y + z * 3; f[3] = 1;
	} else if (kind == 'n') {
		f[0] = x & 1; f[1] = y & 1; f[2] = (x ^ y ^ z) & 1; f[3] = 1;
	} else {
		i[0] = x; i[1] = kind == 'i' ? -y : y; i[2] = x + y + z * 3; i[3] = 1;
	}
	for (c = 0; c < 4; c++) {
		if (kind == 'f' || kind == 'n') {
			float v = c < comps ? f[c] : c == 3 ? 1.0f : 0.0f;
			memcpy(&out[c], &v, 4);
		} else {
			out[c] = c < comps ? (uint32_t)i[c] : c == 3 ? 1 : 0;
		}
	}
}

int main(int argc, char **argv)
{
	VkInstance inst;
	uint32_t n = 8;
	VkPhysicalDevice pd[8];
	VkPhysicalDeviceProperties props;
	VkBuffer b0, b1;
	uint32_t *w0;
	uint8_t *m1;
	unsigned fi, di, total_bad = 0, cases = 0;

	CHECK(vkCreateInstance(&(VkInstanceCreateInfo){
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }, NULL, &inst));
	CHECK(vkEnumeratePhysicalDevices(inst, &n, pd));
	pdev = pd[0];
	vkGetPhysicalDeviceProperties(pdev, &props);
	printf("device: %s\n", props.deviceName);
	CHECK(vkCreateDevice(pdev, &(VkDeviceCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
			.pQueuePriorities = (float[]){ 1.0f } },
		.pEnabledFeatures = &(VkPhysicalDeviceFeatures){
			.shaderStorageImageExtendedFormats = VK_TRUE } }, NULL, &dev));
	vkGetDeviceQueue(dev, 0, 0, &queue);
	CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }, NULL, &pool));

	w0 = (uint32_t *)make_buffer(&b0, W * H * D * 16);
	m1 = make_buffer(&b1, W * H * D * 4);

	for (fi = 0; fi < sizeof(formats) / sizeof(formats[0]); fi++) {
		if (argc > 1 && strcmp(argv[1], formats[fi].name))
			continue;
		printf("%-15s", formats[fi].name);
		for (di = 0; di < 3; di++) {
			const int depth = di ? D : 1;
			VkFormatProperties fp;
			VkImage img;
			VkImageView view;
			VkMemoryRequirements mr;
			VkDeviceMemory mem;
			VkPipeline pipe;
			VkDescriptorPool dpool;
			VkDescriptorSet set;
			VkCommandBuffer cmd;
			char file[128];
			int pass, x, y, z, bad = 0, cbad = -1;

			vkGetPhysicalDeviceFormatProperties(pdev, formats[fi].format, &fp);
			if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
				printf("  %s: no storage support", dims[di]);
				continue;
			}
			CHECK(vkCreateImage(dev, &(VkImageCreateInfo){
				.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
				.imageType = di == 2 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
				.format = formats[fi].format,
				.extent = { W, H, di == 2 ? D : 1 }, .mipLevels = 1,
				.arrayLayers = di == 1 ? D : 1, .samples = VK_SAMPLE_COUNT_1_BIT,
				.tiling = VK_IMAGE_TILING_OPTIMAL,
				.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT },
				NULL, &img));
			vkGetImageMemoryRequirements(dev, img, &mr);
			CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
				.memoryTypeIndex = mem_type(mr.memoryTypeBits, 0) }, NULL, &mem));
			CHECK(vkBindImageMemory(dev, img, mem, 0));
			CHECK(vkCreateImageView(dev, &(VkImageViewCreateInfo){
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
				.viewType = di == 2 ? VK_IMAGE_VIEW_TYPE_3D :
					    di == 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
				.format = formats[fi].format,
				.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, di == 1 ? D : 1 } },
				NULL, &view));
			cmd = begin();
			layout(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
			       VK_IMAGE_LAYOUT_GENERAL);
			run(cmd);
			memset(w0, 0x55, W * H * D * 16);

			for (pass = 0; pass < 2; pass++) {
				snprintf(file, sizeof(file), "si/%s_%s_%s.spv", formats[fi].name,
					 dims[di], pass ? "load" : "store");
				pipe = make_pipe(file, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
				CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
					.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
					.poolSizeCount = 2, .pPoolSizes = (VkDescriptorPoolSize[]){
						{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
						{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } } }, NULL, &dpool));
				CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
					.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
					.descriptorPool = dpool, .descriptorSetCount = 1,
					.pSetLayouts = &dsl }, &set));
				vkUpdateDescriptorSets(dev, 2, (VkWriteDescriptorSet[]){
					{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
					  .dstBinding = 0, .descriptorCount = 1,
					  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
					  .pImageInfo = &(VkDescriptorImageInfo){
						VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL } },
					{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
					  .dstBinding = 1, .descriptorCount = 1,
					  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					  .pBufferInfo = &(VkDescriptorBufferInfo){ b0, 0, VK_WHOLE_SIZE } } },
					0, NULL);
				cmd = begin();
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
				vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1,
							&set, 0, NULL);
				vkCmdDispatch(cmd, W / 4, H / 4, depth);
				run(cmd);
			}
			for (z = 0; z < depth; z++)
				for (y = 0; y < H; y++)
					for (x = 0; x < W; x++) {
						uint32_t want[4];
						expect(formats[fi].kind, formats[fi].comps, x, y, z, want);
						bad += memcmp(want, &w0[(x + W * (y + H * z)) * 4], 16) != 0;
					}
			if (fi == 0) {
				/* the bytes as the copy path sees them */
				memset(m1, 0x11, W * H * D * 4);
				cmd = begin();
				layout(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
				       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
				vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b1, 1,
					&(VkBufferImageCopy){
						.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
								      di == 1 ? D : 1 },
						.imageExtent = { W, H, di == 2 ? D : 1 } });
				run(cmd);
				for (cbad = 0, z = 0; z < depth; z++)
					for (y = 0; y < H; y++)
						for (x = 0; x < W; x++) {
							const uint8_t want[4] = { (x & 1) * 255, (y & 1) * 255,
								((x ^ y ^ z) & 1) * 255, 255 };
							cbad += memcmp(want, m1 + (x + W * (y + H * z)) * 4, 4) != 0;
						}
			}
			printf("  %s: %d", dims[di], bad);
			if (cbad >= 0)
				printf(" (copy %d)", cbad);
			total_bad += bad + (cbad > 0 ? cbad : 0);
			cases++;
		}
		printf("\n");
		fflush(stdout);
	}
	printf("texels wrong per case shown above; %u cases, %u wrong in total\n", cases, total_bad);
	return 0;
}
