/* Offscreen Vulkan test: arrays of descriptors indexed at run time
 * (shaderSampledImage/StorageImage/UniformBuffer/StorageBufferArrayDynamicIndexing).
 * One compute dispatch of dyn.comp.spv, or with VKDYN_FRAG=1 one pixel drawn
 * with dyn.vert.spv + dyn.frag.spv (the same checks in a fragment shader);
 * every index comes from push constants (a permutation, so a wrong unit
 * gives a wrong colour).
 *  Y1 combined image samplers ctex[4]
 *  Y2 separate textures stex[3] with separate samplers smp[2] (smp[0] clamps
 *     to the edge, smp[1] to an opaque white border; sampled outside)
 *  Y3 storage buffers sb[3], Y4 uniform buffers ub[3]
 *  Y5 storage images simg[3]
 *  Y6 array textures carr[2], two layers each
 *  Y7 textureSize(ctex[i]), Y8 texelFetch(ctex[i])
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

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

static uint32_t *make_buffer(VkBuffer *buf, VkDeviceSize size, VkBufferUsageFlags usage)
{
	VkMemoryRequirements mr;
	VkDeviceMemory mem;
	uint32_t *map;
	CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo){
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage },
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

static void layout(VkCommandBuffer cmd, VkImage img, uint32_t layers,
		   VkImageLayout from, VkImageLayout to)
{
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1,
		&(VkImageMemoryBarrier){
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
			.oldLayout = from, .newLayout = to,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = img,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers } });
}

/* colour number c: distinct bytes, alpha 255 */
static uint32_t colour(int c)
{
	return ((c * 37 + 16) & 0xff) | (((200 - c * 5) & 0xff) << 8) | ((c & 0xff) << 16) |
	       0xff000000u;
}

/* a w x 4 RGBA8 image, every layer cleared to its own colour, left in final */
static VkImageView make_image(uint32_t w, uint32_t layers, int c0, VkImageUsageFlags usage,
			      VkImageLayout final)
{
	VkImage img;
	VkImageView view;
	VkMemoryRequirements mr;
	VkDeviceMemory mem;
	VkCommandBuffer cmd;
	CHECK(vkCreateImage(dev, &(VkImageCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { w, 4, 1 }, .mipLevels = 1, .arrayLayers = layers,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT }, NULL, &img));
	vkGetImageMemoryRequirements(dev, img, &mr);
	CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits, 0) }, NULL, &mem));
	CHECK(vkBindImageMemory(dev, img, mem, 0));
	cmd = begin();
	layout(cmd, img, layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	for (uint32_t l = 0; l < layers; l++) {
		uint32_t v = colour(c0 + l);
		VkClearColorValue cc = { .float32 = { (v & 0xff) / 255.0f, ((v >> 8) & 0xff) / 255.0f,
						     ((v >> 16) & 0xff) / 255.0f, 1.0f } };
		vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1,
			&(VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, l, 1 });
	}
	layout(cmd, img, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, final);
	run(cmd);
	CHECK(vkCreateImageView(dev, &(VkImageViewCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
		.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers } }, NULL, &view));
	return view;
}

static VkSampler make_sampler(VkSamplerAddressMode mode)
{
	VkSampler s;
	CHECK(vkCreateSampler(dev, &(VkSamplerCreateInfo){
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
		.addressModeU = mode, .addressModeV = mode, .addressModeW = mode,
		.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE }, NULL, &s));
	return s;
}

static VkShaderModule load_spv(const char *file)
{
	static uint32_t code[16384];
	VkShaderModule mod;
	FILE *f = fopen(file, "rb");
	size_t size;
	if (!f) {
		fprintf(stderr, "%s missing\n", file);
		exit(1);
	}
	size = fread(code, 1, sizeof(code), f);
	fclose(f);
	CHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = size, .pCode = code }, NULL, &mod));
	return mod;
}

/* a 1x1 colour target, its render pass and a pipeline drawing one triangle */
static VkRenderPass rp;
static VkFramebuffer fb;

static VkPipeline make_gfx(VkPipelineLayout pl)
{
	VkImage img;
	VkImageView view;
	VkMemoryRequirements mr;
	VkDeviceMemory mem;
	VkPipeline pipe;
	CHECK(vkCreateImage(dev, &(VkImageCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { 1, 1, 1 }, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT }, NULL, &img));
	vkGetImageMemoryRequirements(dev, img, &mr);
	CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits, 0) }, NULL, &mem));
	CHECK(vkBindImageMemory(dev, img, mem, 0));
	CHECK(vkCreateImageView(dev, &(VkImageViewCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
		.viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } }, NULL, &view));
	CHECK(vkCreateRenderPass(dev, &(VkRenderPassCreateInfo){
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
		.pAttachments = &(VkAttachmentDescription){
			.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
		.subpassCount = 1, .pSubpasses = &(VkSubpassDescription){
			.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
			.colorAttachmentCount = 1, .pColorAttachments = &(VkAttachmentReference){
				0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } } }, NULL, &rp));
	CHECK(vkCreateFramebuffer(dev, &(VkFramebufferCreateInfo){
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
		.attachmentCount = 1, .pAttachments = &view, .width = 1, .height = 1,
		.layers = 1 }, NULL, &fb));
	CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &(VkGraphicsPipelineCreateInfo){
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
		.pStages = (VkPipelineShaderStageCreateInfo[]){
			{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			  .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = load_spv("dyn.vert.spv"),
			  .pName = "main" },
			{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			  .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = load_spv("dyn.frag.spv"),
			  .pName = "main" } },
		.pVertexInputState = &(VkPipelineVertexInputStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO },
		.pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST },
		.pViewportState = &(VkPipelineViewportStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1, .pViewports = &(VkViewport){ 0, 0, 1, 1, 0, 1 },
			.scissorCount = 1, .pScissors = &(VkRect2D){ { 0, 0 }, { 1, 1 } } },
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
		.layout = pl, .renderPass = rp }, NULL, &pipe));
	return pipe;
}

static int bad;

static void expect(const char *what, int idx, uint32_t got, uint32_t want)
{
	if (got != want) {
		printf("  %s: o[%d] = %08x, expect %08x\n", what, idx, got, want);
		bad++;
	}
}

int main(void)
{
	static const uint32_t p[8] = { 2, 0, 3, 1, 1, 0, 2, 1 };
	const int frag = getenv("VKDYN_FRAG") != NULL;
	VkInstance inst;
	uint32_t n = 8;
	VkPhysicalDevice pd[8];
	VkPhysicalDeviceProperties props;
	VkImageView ctex[4], stex[3], simg[3], carr[2];
	VkSampler edge, border;
	VkBuffer sb[3], ub[3], out;
	uint32_t *o;
	VkDescriptorSetLayout dsl;
	VkPipelineLayout pl;
	VkPipeline pipe;
	VkDescriptorPool dpool;
	VkDescriptorSet set;
	VkCommandBuffer cmd;
	VkDescriptorImageInfo ii_c[4], ii_s[3], ii_smp[2], ii_si[3], ii_a[2];
	VkDescriptorBufferInfo bi_sb[3], bi_ub[3];
	const VkShaderStageFlags cs = frag ? VK_SHADER_STAGE_FRAGMENT_BIT :
					     VK_SHADER_STAGE_COMPUTE_BIT;
	const VkPipelineBindPoint bp = frag ? VK_PIPELINE_BIND_POINT_GRAPHICS :
					      VK_PIPELINE_BIND_POINT_COMPUTE;
	int i, b0;

	CHECK(vkCreateInstance(&(VkInstanceCreateInfo){
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }, NULL, &inst));
	CHECK(vkEnumeratePhysicalDevices(inst, &n, pd));
	pdev = pd[0];
	vkGetPhysicalDeviceProperties(pdev, &props);
	printf("device: %s, %s stage\n", props.deviceName, frag ? "fragment" : "compute");
	CHECK(vkCreateDevice(pdev, &(VkDeviceCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
			.pQueuePriorities = (float[]){ 1.0f } },
		.pEnabledFeatures = &(VkPhysicalDeviceFeatures){
			.shaderSampledImageArrayDynamicIndexing = VK_TRUE,
			.shaderStorageImageArrayDynamicIndexing = VK_TRUE,
			.shaderUniformBufferArrayDynamicIndexing = VK_TRUE,
			.shaderStorageBufferArrayDynamicIndexing = VK_TRUE,
			.fragmentStoresAndAtomics = VK_TRUE } }, NULL, &dev));
	vkGetDeviceQueue(dev, 0, 0, &queue);
	CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }, NULL, &pool));

	edge = make_sampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
	border = make_sampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
	for (i = 0; i < 4; i++) {
		ctex[i] = make_image(4 * (i + 1), 1, i, VK_IMAGE_USAGE_SAMPLED_BIT,
				     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		ii_c[i] = (VkDescriptorImageInfo){ edge, ctex[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	}
	for (i = 0; i < 3; i++) {
		stex[i] = make_image(4, 1, 10 + i, VK_IMAGE_USAGE_SAMPLED_BIT,
				     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		ii_s[i] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, stex[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		simg[i] = make_image(4, 1, 20 + i, VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_LAYOUT_GENERAL);
		ii_si[i] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, simg[i], VK_IMAGE_LAYOUT_GENERAL };
		*make_buffer(&sb[i], 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) = 1000 + i;
		*make_buffer(&ub[i], 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) = 2000 + i;
		bi_sb[i] = (VkDescriptorBufferInfo){ sb[i], 0, VK_WHOLE_SIZE };
		bi_ub[i] = (VkDescriptorBufferInfo){ ub[i], 0, VK_WHOLE_SIZE };
	}
	ii_smp[0] = (VkDescriptorImageInfo){ edge, VK_NULL_HANDLE, 0 };
	ii_smp[1] = (VkDescriptorImageInfo){ border, VK_NULL_HANDLE, 0 };
	for (i = 0; i < 2; i++) {
		carr[i] = make_image(4, 2, 30 + 2 * i, VK_IMAGE_USAGE_SAMPLED_BIT,
				     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		ii_a[i] = (VkDescriptorImageInfo){ edge, carr[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	}
	o = make_buffer(&out, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

	CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 8,
		.pBindings = (VkDescriptorSetLayoutBinding[]){
			{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4, cs, NULL },
			{ 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3, cs, NULL },
			{ 2, VK_DESCRIPTOR_TYPE_SAMPLER, 2, cs, NULL },
			{ 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3, cs, NULL },
			{ 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3, cs, NULL },
			{ 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3, cs, NULL },
			{ 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, cs, NULL },
			{ 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs, NULL } } }, NULL, &dsl));
	CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1,
		.pPushConstantRanges = &(VkPushConstantRange){ cs, 0, sizeof(p) } }, NULL, &pl));
	if (frag)
		pipe = make_gfx(pl);
	else
		CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				   .stage = cs, .module = load_spv("dyn.comp.spv"), .pName = "main" },
			.layout = pl }, NULL, &pipe));
	printf("pipeline created\n");

	CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
		.poolSizeCount = 6, .pPoolSizes = (VkDescriptorPoolSize[]){
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6 },
			{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 2 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 } } }, NULL, &dpool));
	CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl }, &set));
#define W_IMG(b, cnt, t, info) { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, \
	.dstBinding = b, .descriptorCount = cnt, .descriptorType = t, .pImageInfo = info }
#define W_BUF(b, cnt, t, info) { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, \
	.dstBinding = b, .descriptorCount = cnt, .descriptorType = t, .pBufferInfo = info }
	vkUpdateDescriptorSets(dev, 8, (VkWriteDescriptorSet[]){
		W_IMG(0, 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, ii_c),
		W_IMG(1, 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, ii_s),
		W_IMG(2, 2, VK_DESCRIPTOR_TYPE_SAMPLER, ii_smp),
		W_BUF(3, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bi_sb),
		W_BUF(4, 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, bi_ub),
		W_IMG(5, 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, ii_si),
		W_IMG(6, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, ii_a),
		W_BUF(7, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		      (&(VkDescriptorBufferInfo){ out, 0, VK_WHOLE_SIZE })) }, 0, NULL);

	cmd = begin();
	if (frag)
		vkCmdBeginRenderPass(cmd, &(VkRenderPassBeginInfo){
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
			.framebuffer = fb, .renderArea = { { 0, 0 }, { 1, 1 } } },
			VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cmd, bp, pipe);
	vkCmdBindDescriptorSets(cmd, bp, pl, 0, 1, &set, 0, NULL);
	vkCmdPushConstants(cmd, pl, cs, 0, sizeof(p), p);
	if (frag) {
		vkCmdDraw(cmd, 3, 1, 0, 0);
		vkCmdEndRenderPass(cmd);
	} else {
		vkCmdDispatch(cmd, 1, 1, 1);
	}
	run(cmd);

#define GROUP(name, from, to, body) do { b0 = bad; for (i = from; i < to; i++) { body; } \
	printf("%s: %s\n", name, bad == b0 ? "correct" : "WRONG"); } while (0)
	GROUP("Y1 combined image samplers", 0, 4, expect("Y1", i, o[i], colour(p[i])));
	b0 = bad;
	expect("Y2", 4, o[4], 0xffffffffu);			/* smp[1]: border */
	expect("Y2", 5, o[5], colour(10 + p[5]));		/* smp[0]: edge */
	expect("Y2", 6, o[6], colour(10 + p[6]));
	printf("Y2 separate textures and samplers: %s\n", bad == b0 ? "correct" : "WRONG");
	GROUP("Y3 storage buffers", 0, 3, expect("Y3", 7 + i, o[7 + i], 1000 + p[4 + i]));
	GROUP("Y4 uniform buffers", 0, 3, expect("Y4", 10 + i, o[10 + i], 2000 + p[4 + i]));
	GROUP("Y5 storage images", 0, 3, expect("Y5", 13 + i, o[13 + i], colour(20 + p[4 + i])));
	b0 = bad;
	expect("Y6", 16, o[16], colour(30 + 2 * p[7]));
	expect("Y6", 17, o[17], colour(31 + 2 * p[7]));
	expect("Y6", 18, o[18], colour(30 + 2 * (p[7] ^ 1)));
	expect("Y6", 19, o[19], colour(31 + 2 * (p[7] ^ 1)));
	printf("Y6 array textures: %s\n", bad == b0 ? "correct" : "WRONG");
	GROUP("Y7 textureSize", 0, 4, expect("Y7", 20 + i, o[20 + i], 4 * (p[i] + 1)));
	GROUP("Y8 texelFetch", 0, 4, expect("Y8", 24 + i, o[24 + i], colour(p[i])));
	printf("%d of 28 words wrong\n", bad);
	return bad != 0;
}
