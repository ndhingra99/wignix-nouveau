/* Offscreen Vulkan test, step 1: clear a colour image through a render
 * pass, copy it to a buffer, read the buffer back.  No shaders, no window.
 * With "tri" as argument a second pass draws a triangle from the SPIR-V in
 * tri.vert.spv / tri.frag.spv (current directory) before the copy.
 * Prints a few pixels and how many of them have the expected colour.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define W 64
#define H 64
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s failed: %d\n", #x, r_); exit(1); } } while (0)

static VkPhysicalDevice pdev;
static VkDevice dev;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pdev, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	fprintf(stderr, "no memory type\n");
	exit(1);
}

static VkShaderModule load_spv(const char *path)
{
	FILE *f = fopen(path, "rb");
	static uint32_t code[16384];
	size_t n;
	VkShaderModule m;
	if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
	n = fread(code, 1, sizeof(code), f);
	fclose(f);
	CHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = n, .pCode = code }, NULL, &m));
	return m;
}

int main(int argc, char **argv)
{
	const int tri = argc > 1 && !strcmp(argv[1], "tri");
	VkInstance inst;
	uint32_t n = 8, qf = 0;
	VkPhysicalDevice pd[8];
	VkPhysicalDeviceProperties props;
	VkQueue queue;
	VkImage img;
	VkDeviceMemory imem, bmem;
	VkImageView view;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkBuffer buf;
	VkCommandPool pool;
	VkCommandBuffer cmd;
	VkMemoryRequirements mr;
	VkPipeline pipe = VK_NULL_HANDLE;
	uint8_t *map;
	int ok = 0, inside = 0;

	CHECK(vkCreateInstance(&(VkInstanceCreateInfo){
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &(VkApplicationInfo){
			.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
			.apiVersion = VK_API_VERSION_1_0 } }, NULL, &inst));
	CHECK(vkEnumeratePhysicalDevices(inst, &n, pd));
	if (!n) { fprintf(stderr, "no device\n"); return 1; }
	pdev = pd[0];
	vkGetPhysicalDeviceProperties(pdev, &props);
	printf("device: %s\n", props.deviceName);

	CHECK(vkCreateDevice(pdev, &(VkDeviceCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueFamilyIndex = qf, .queueCount = 1,
			.pQueuePriorities = (float[]){ 1.0f } } }, NULL, &dev));
	vkGetDeviceQueue(dev, qf, 0, &queue);

	CHECK(vkCreateImage(dev, &(VkImageCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT },
		NULL, &img));
	vkGetImageMemoryRequirements(dev, img, &mr);
	CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits, 0) }, NULL, &imem));
	CHECK(vkBindImageMemory(dev, img, imem, 0));
	CHECK(vkCreateImageView(dev, &(VkImageViewCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } }, NULL, &view));

	CHECK(vkCreateRenderPass(dev, &(VkRenderPassCreateInfo){
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
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
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = rp, .attachmentCount = 1, .pAttachments = &view,
		.width = W, .height = H, .layers = 1 }, NULL, &fb));

	CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo){
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = W * H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT }, NULL, &buf));
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) },
		NULL, &bmem));
	CHECK(vkBindBufferMemory(dev, buf, bmem, 0));
	CHECK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&map));
	memset(map, 0x55, W * H * 4);

	if (tri) {
		VkPipelineLayout layout;
		VkShaderModule vs = load_spv("tri.vert.spv"), fs = load_spv("tri.frag.spv");
		CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO }, NULL, &layout));
		CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &(VkGraphicsPipelineCreateInfo){
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.stageCount = 2,
			.pStages = (VkPipelineShaderStageCreateInfo[]){
				{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				  .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
				{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				  .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } },
			.pVertexInputState = &(VkPipelineVertexInputStateCreateInfo){
				.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO },
			.pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo){
				.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
				.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST },
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
	}

	CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = qf }, NULL, &pool));
	CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1 }, &cmd));
	CHECK(vkBeginCommandBuffer(cmd, &(VkCommandBufferBeginInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }));
	vkCmdBeginRenderPass(cmd, &(VkRenderPassBeginInfo){
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rp, .framebuffer = fb,
		.renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 1,
		.pClearValues = &(VkClearValue){ .color = { .float32 = { 1.0f, 0.5f, 0.25f, 1.0f } } } },
		VK_SUBPASS_CONTENTS_INLINE);
	if (tri) {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdDraw(cmd, 3, 1, 0, 0);
	}
	vkCmdEndRenderPass(cmd);
	vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1,
		&(VkBufferImageCopy){
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageExtent = { W, H, 1 } });
	CHECK(vkEndCommandBuffer(cmd));
	CHECK(vkQueueSubmit(queue, 1, &(VkSubmitInfo){
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1, .pCommandBuffers = &cmd }, VK_NULL_HANDLE));
	CHECK(vkQueueWaitIdle(queue));

	/* expected: clear colour (255,128,64,255); the triangle is (0,255,0,255)
	 * and covers the lower left half */
	for (int i = 0; i < W * H; i++) {
		const uint8_t *p = map + i * 4;
		if (p[0] == 255 && (p[1] == 127 || p[1] == 128) && (p[2] == 63 || p[2] == 64) && p[3] == 255)
			ok++;
		if (p[0] == 0 && p[1] == 255 && p[2] == 0 && p[3] == 255)
			inside++;
	}
	printf("pixel 0: %d,%d,%d,%d  centre: %d,%d,%d,%d  last: %d,%d,%d,%d\n",
	       map[0], map[1], map[2], map[3],
	       map[(H / 2 * W + W / 2) * 4], map[(H / 2 * W + W / 2) * 4 + 1],
	       map[(H / 2 * W + W / 2) * 4 + 2], map[(H / 2 * W + W / 2) * 4 + 3],
	       map[(W * H - 1) * 4], map[(W * H - 1) * 4 + 1], map[(W * H - 1) * 4 + 2],
	       map[(W * H - 1) * 4 + 3]);
	printf("clear-colour pixels: %d, triangle-colour pixels: %d, of %d\n", ok, inside, W * H);
	return 0;
}
