/* Offscreen Vulkan test, step 8: graphics stages beyond a plain triangle,
 * each into a 64x64 RGBA8 target that is copied back and counted.
 *  G1 a fragment shader writes a storage buffer and a storage image.
 *  G2 a geometry shader turns one point into a full-screen triangle.
 *  G3 a tessellated triangle covering the lower-left half.
 *  G4 four instanced quads: vkCmdDrawIndexed, vkCmdDrawIndexedIndirect,
 *     vkCmdDrawIndirect.
 * Shaders in ./gfx (glslangValidator -V <file> -o <file>.spv).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define W 64
#define H 48
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
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
			VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT },
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

#define W 64
#define H 64

static VkRenderPass rp;
static VkFramebuffer fb;
static VkImage target;
static VkBuffer bpix;
static uint8_t *pix;

static VkShaderModule load_spv(const char *name)
{
	static uint32_t code[16384];
	char path[128];
	VkShaderModule mod;
	FILE *f;
	size_t size;
	snprintf(path, sizeof(path), "gfx/%s.spv", name);
	if (!(f = fopen(path, "rb"))) {
		fprintf(stderr, "%s missing\n", path);
		exit(1);
	}
	size = fread(code, 1, sizeof(code), f);
	fclose(f);
	CHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = size, .pCode = code }, NULL, &mod));
	return mod;
}

/* stage files in the order vertex, tess control, tess evaluation, geometry,
 * fragment; NULL where there is none
 */
static VkPipeline make_gfx(const char *files[5], VkPrimitiveTopology topo,
			   VkPipelineLayout layout)
{
	static const VkShaderStageFlagBits bits[5] = {
		VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
		VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
		VK_SHADER_STAGE_FRAGMENT_BIT };
	VkPipelineShaderStageCreateInfo stages[5];
	VkPipeline pipe;
	uint32_t i, n = 0;
	for (i = 0; i < 5; i++) {
		if (!files[i])
			continue;
		stages[n++] = (VkPipelineShaderStageCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = bits[i], .module = load_spv(files[i]), .pName = "main" };
	}
	CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &(VkGraphicsPipelineCreateInfo){
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = n, .pStages = stages,
		.pVertexInputState = &(VkPipelineVertexInputStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO },
		.pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = topo },
		.pTessellationState = files[1] ? &(VkPipelineTessellationStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
			.patchControlPoints = 3 } : NULL,
		.pViewportState = &(VkPipelineViewportStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1, .pViewports = &(VkViewport){ 0, 0, W, H, 0, 1 },
			.scissorCount = 1, .pScissors = &(VkRect2D){ { 0, 0 }, { W, H } } },
		.pRasterizationState = &(VkPipelineRasterizationStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
			.lineWidth = 1.0f },
		.pMultisampleState = &(VkPipelineMultisampleStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT },
		.pColorBlendState = &(VkPipelineColorBlendStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = 1,
			.pAttachments = &(VkPipelineColorBlendAttachmentState){
				.colorWriteMask = 0xf } },
		.layout = layout, .renderPass = rp }, NULL, &pipe));
	return pipe;
}

static VkCommandBuffer begin_pass(void)
{
	VkCommandBuffer cmd = begin();
	vkCmdBeginRenderPass(cmd, &(VkRenderPassBeginInfo){
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
		.framebuffer = fb, .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 1,
		.pClearValues = &(VkClearValue){ .color = { .float32 = { 0, 0, 0, 1 } } } },
		VK_SUBPASS_CONTENTS_INLINE);
	return cmd;
}

/* ends the pass, copies the target back, returns how many pixels have the
 * given colour
 */
static int end_pass(VkCommandBuffer cmd, uint8_t r, uint8_t g, uint8_t b)
{
	int i, n = 0;
	vkCmdEndRenderPass(cmd);
	vkCmdCopyImageToBuffer(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bpix, 1,
		&(VkBufferImageCopy){ .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
				      .imageExtent = { W, H, 1 } });
	run(cmd);
	for (i = 0; i < W * H; i++)
		n += pix[i * 4] == r && pix[i * 4 + 1] == g && pix[i * 4 + 2] == b;
	return n;
}

int main(void)
{
	VkInstance inst;
	uint32_t n = 8;
	VkPhysicalDevice pd[8];
	VkPhysicalDeviceProperties props;
	VkDeviceMemory mem;
	VkMemoryRequirements mr;
	VkImageView view, sview;
	VkImage simg;
	VkPipelineLayout pl0, pl1;
	VkDescriptorSetLayout dsl;
	VkDescriptorPool dpool;
	VkDescriptorSet set;
	VkBuffer bs, bi, bx, bsc;
	uint32_t *ws, *wi;
	uint16_t *wx;
	uint8_t *msc;
	VkPipeline pipe;
	VkCommandBuffer cmd;
	int i, cnt, bad;

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
			.geometryShader = VK_TRUE, .tessellationShader = VK_TRUE,
			.fragmentStoresAndAtomics = VK_TRUE } }, NULL, &dev));
	vkGetDeviceQueue(dev, 0, 0, &queue);
	CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }, NULL, &pool));

	/* target, render pass, framebuffer */
	CHECK(vkCreateImage(dev, &(VkImageCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT },
		NULL, &target));
	vkGetImageMemoryRequirements(dev, target, &mr);
	CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits, 0) }, NULL, &mem));
	CHECK(vkBindImageMemory(dev, target, mem, 0));
	CHECK(vkCreateImageView(dev, &(VkImageViewCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = target,
		.viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } }, NULL, &view));
	CHECK(vkCreateRenderPass(dev, &(VkRenderPassCreateInfo){
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
		.pAttachments = &(VkAttachmentDescription){
			.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL },
		.subpassCount = 1,
		.pSubpasses = &(VkSubpassDescription){
			.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
			.colorAttachmentCount = 1,
			.pColorAttachments = &(VkAttachmentReference){
				0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } } }, NULL, &rp));
	CHECK(vkCreateFramebuffer(dev, &(VkFramebufferCreateInfo){
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
		.attachmentCount = 1, .pAttachments = &view,
		.width = W, .height = H, .layers = 1 }, NULL, &fb));
	pix = make_buffer(&bpix, W * H * 4);

	CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO }, NULL, &pl0));

	/* G1 */
	ws = (uint32_t *)make_buffer(&bs, W * H * 4);
	msc = make_buffer(&bsc, W * H * 4);
	CHECK(vkCreateImage(dev, &(VkImageCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT },
		NULL, &simg));
	vkGetImageMemoryRequirements(dev, simg, &mr);
	CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits, 0) }, NULL, &mem));
	CHECK(vkBindImageMemory(dev, simg, mem, 0));
	CHECK(vkCreateImageView(dev, &(VkImageViewCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = simg,
		.viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } }, NULL, &sview));
	CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2,
		.pBindings = (VkDescriptorSetLayoutBinding[]){
			{ .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			  .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
			{ .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			  .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT } } },
		NULL, &dsl));
	CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &dsl }, NULL, &pl1));
	CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
		.poolSizeCount = 2, .pPoolSizes = (VkDescriptorPoolSize[]){
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 } } }, NULL, &dpool));
	CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl }, &set));
	vkUpdateDescriptorSets(dev, 2, (VkWriteDescriptorSet[]){
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
		  .dstBinding = 0, .descriptorCount = 1,
		  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		  .pBufferInfo = &(VkDescriptorBufferInfo){ bs, 0, VK_WHOLE_SIZE } },
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
		  .dstBinding = 1, .descriptorCount = 1,
		  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		  .pImageInfo = &(VkDescriptorImageInfo){
			VK_NULL_HANDLE, sview, VK_IMAGE_LAYOUT_GENERAL } } }, 0, NULL);
	cmd = begin();
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1,
		&(VkImageMemoryBarrier){
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = 0x7ff,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = simg,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } });
	run(cmd);
	pipe = make_gfx((const char *[5]){ "full.vert", NULL, NULL, NULL, "store.frag" },
			VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, pl1);
	cmd = begin_pass();
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl1, 0, 1, &set, 0, NULL);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	cnt = end_pass(cmd, 0, 255, 0);
	for (bad = 0, i = 0; i < W * H; i++)
		bad += ws[i] != (uint32_t)((i % W) | ((i / W) << 8) | 0xab0000);
	printf("G1 fragment stores: %d of %d pixels drawn, %d buffer words wrong", cnt, W * H, bad);
	cmd = begin();
	vkCmdCopyImageToBuffer(cmd, simg, VK_IMAGE_LAYOUT_GENERAL, bsc, 1,
		&(VkBufferImageCopy){ .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
				      .imageExtent = { W, H, 1 } });
	run(cmd);
	for (bad = 0, i = 0; i < W * H; i++) {
		const uint8_t want[4] = { ((i % W) & 1) * 255, ((i / W) & 1) * 255, 0, 255 };
		bad += memcmp(want, msc + i * 4, 4) != 0;
	}
	printf(", %d image texels wrong\n", bad);

	/* G2 */
	pipe = make_gfx((const char *[5]){ "point.vert", NULL, NULL, "full.geom", "col.frag" },
			VK_PRIMITIVE_TOPOLOGY_POINT_LIST, pl0);
	cmd = begin_pass();
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdDraw(cmd, 1, 1, 0, 0);
	cnt = end_pass(cmd, 0, 0, 255);
	printf("G2 geometry shader: %d of %d pixels\n", cnt, W * H);

	/* G3 */
	pipe = make_gfx((const char *[5]){ "half.vert", "half.tesc", "half.tese", NULL, "col.frag" },
			VK_PRIMITIVE_TOPOLOGY_PATCH_LIST, pl0);
	cmd = begin_pass();
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	cnt = end_pass(cmd, 255, 0, 0);
	printf("G3 tessellation: %d pixels (expect 2016, the untessellated triangle)\n", cnt);

	/* G4 */
	wx = (uint16_t *)make_buffer(&bx, 64);
	wi = (uint32_t *)make_buffer(&bi, 256);
	memcpy(wx, (uint16_t[6]){ 0, 1, 2, 2, 1, 3 }, 12);
	memcpy(wi, (uint32_t[5]){ 6, 4, 0, 0, 0 }, 20);      /* indexed indirect */
	memcpy(wi + 8, (uint32_t[4]){ 6, 4, 0, 0 }, 16);      /* indirect */
	pipe = make_gfx((const char *[5]){ "quad.vert", NULL, NULL, NULL, "col.frag" },
			VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, pl0);
	cmd = begin_pass();
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdBindIndexBuffer(cmd, bx, 0, VK_INDEX_TYPE_UINT16);
	vkCmdDrawIndexed(cmd, 6, 4, 0, 0, 0);
	cnt = end_pass(cmd, 255, 255, 0);
	printf("G4 instanced quads: indexed %d", cnt);
	cmd = begin_pass();
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdBindIndexBuffer(cmd, bx, 0, VK_INDEX_TYPE_UINT16);
	vkCmdDrawIndexedIndirect(cmd, bi, 0, 1, 20);
	cnt = end_pass(cmd, 255, 255, 0);
	printf(", indexed indirect %d", cnt);
	pipe = make_gfx((const char *[5]){ "quadni.vert", NULL, NULL, NULL, "col.frag" },
			VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, pl0);
	cmd = begin_pass();
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdDrawIndirect(cmd, bi, 32, 1, 16);
	cnt = end_pass(cmd, 0, 255, 255);
	printf(", indirect %d (expect 1024 each)\n", cnt);
	return 0;
}
