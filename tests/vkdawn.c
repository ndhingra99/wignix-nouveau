/* Offscreen Vulkan test: the device features Dawn (WebGPU in Chromium)
 * requires, run on every device the loader offers whose name contains "NVK"
 * or "llvmpipe" (set VK_ICD_FILENAMES to both ICD files, colon separated).
 * Exact results are checked on each device; the BC decodes are compared
 * between NVK and lavapipe.  Shaders in dawn/, compiled to .spv.
 *  Z1 textureCompressionBC: random 8x8 images in twelve BC formats
 *  Z2 robustBufferAccess: storage and uniform reads and writes past a
 *     16-word binding (a read may return zero or any value from the memory
 *     bound to the buffer; a write must not land outside the binding);
 *     vertex attributes past the end of the buffer
 *  Z3 imageCubeArray: the centre of each face of a two-cube array
 *  Z4 sampleRateShading: gl_SampleID (a), minSampleShading = 1 (b)
 *  Z5 independentBlend: ADD on attachment 0, MIN on attachment 1
 *  Z6 depthBiasClamp: a huge constant bias clamped to +-0.01
 *  Z7 fullDrawIndexUint32: indices above 2^24
 *  Z8 limits against WebGPU's minimums (NVK only)
 *  Z9 push constants in vertex and fragment shaders (after the compute
 *     cases above: on Fermi this is where compute leaves the 3D root table
 *     unreadable, notes 2026-11-24)
 * Switches: VKDAWN_ONLY=<case digits> runs only those cases ("0" adds Z0, a
 * trivial dispatch); VKDAWN_PCMAP=1 prints all 32 push constant words a
 * vertex shader sees, VKDAWN_SAMECB=1 with a dispatch in the same command
 * buffer; VKDAWN_DUMP=1 prints the BC texels of formats that differ.
 */
#include <math.h>
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

/* per device (0 NVK, 1 lavapipe), per BC format: 64 texels x 4 floats */
#define NBC 12
static float bcres[2][NBC][256];
static int bcok[2][NBC];
static int have[2];

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pdev, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	exit(1);
}

/* host-visible buffer of buf_size bytes in an allocation of mem_size bytes
 * (the rest of the allocation lies past the end of the buffer)
 */
static void *make_buffer2(VkBuffer *buf, VkDeviceSize buf_size, VkDeviceSize mem_size,
			  uint8_t fill)
{
	VkMemoryRequirements mr;
	VkDeviceMemory mem;
	void *map;
	CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo){
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = buf_size,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
			 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
			 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT },
		NULL, buf));
	vkGetBufferMemoryRequirements(dev, *buf, &mr);
	if (mem_size < mr.size)
		mem_size = mr.size;
	CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mem_size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) },
		NULL, &mem));
	CHECK(vkBindBufferMemory(dev, *buf, mem, 0));
	CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &map));
	memset(map, fill, mem_size);
	return map;
}

static void *make_buffer(VkBuffer *buf, VkDeviceSize size)
{
	return make_buffer2(buf, size, size, 0x55);
}

static VkImage make_image(VkFormat fmt, uint32_t w, uint32_t h, uint32_t layers,
			  VkSampleCountFlagBits samples, VkImageUsageFlags usage,
			  VkImageCreateFlags flags)
{
	VkImage img;
	VkMemoryRequirements mr;
	VkDeviceMemory mem;
	CHECK(vkCreateImage(dev, &(VkImageCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags,
		.imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { w, h, 1 },
		.mipLevels = 1, .arrayLayers = layers, .samples = samples,
		.tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage }, NULL, &img));
	vkGetImageMemoryRequirements(dev, img, &mr);
	CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits, 0) }, NULL, &mem));
	CHECK(vkBindImageMemory(dev, img, mem, 0));
	return img;
}

static VkImageView make_view(VkImage img, VkImageViewType type, VkFormat fmt,
			     VkImageAspectFlags aspect, uint32_t layers)
{
	VkImageView view;
	CHECK(vkCreateImageView(dev, &(VkImageViewCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
		.viewType = type, .format = fmt,
		.subresourceRange = { aspect, 0, 1, 0, layers } }, NULL, &view));
	return view;
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
		   uint32_t layers, VkImageLayout from, VkImageLayout to)
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
			.image = img, .subresourceRange = { aspect, 0, 1, 0, layers } });
}

static VkShaderModule load_spv(const char *name)
{
	static uint32_t code[16384];
	char path[128];
	VkShaderModule mod;
	FILE *f;
	size_t size;
	snprintf(path, sizeof(path), "dawn/%s.spv", name);
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

/* descriptor set layout + set with n bindings 0..n-1 of the given types */
static VkDescriptorSetLayout dsl;
static VkDescriptorSet set;
static VkPipelineLayout pl;

static void make_set(int n, const VkDescriptorType *types, VkShaderStageFlags stages,
		     uint32_t push_size, VkShaderStageFlags push_stages)
{
	VkDescriptorSetLayoutBinding b[8];
	VkDescriptorPoolSize ps[8];
	VkDescriptorPool dp;
	for (int i = 0; i < n; i++) {
		b[i] = (VkDescriptorSetLayoutBinding){ i, types[i], 1, stages, NULL };
		ps[i] = (VkDescriptorPoolSize){ types[i], 1 };
	}
	CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = n, .pBindings = b }, NULL, &dsl));
	CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = n ? 1 : 0, .pSetLayouts = &dsl,
		.pushConstantRangeCount = push_size ? 1 : 0,
		.pPushConstantRanges = &(VkPushConstantRange){ push_stages, 0, push_size } },
		NULL, &pl));
	if (!n)
		return;
	CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
		.poolSizeCount = n, .pPoolSizes = ps }, NULL, &dp));
	CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl }, &set));
}

static void set_buf(uint32_t binding, VkDescriptorType type, VkBuffer buf,
		    VkDeviceSize off, VkDeviceSize range)
{
	vkUpdateDescriptorSets(dev, 1, &(VkWriteDescriptorSet){
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
		.dstBinding = binding, .descriptorCount = 1, .descriptorType = type,
		.pBufferInfo = &(VkDescriptorBufferInfo){ buf, off, range } }, 0, NULL);
}

static void set_img(uint32_t binding, VkSampler samp, VkImageView view)
{
	vkUpdateDescriptorSets(dev, 1, &(VkWriteDescriptorSet){
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
		.dstBinding = binding, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &(VkDescriptorImageInfo){ samp, view,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } }, 0, NULL);
}

static void compute(const char *name, uint32_t gx, const void *push, uint32_t push_size)
{
	VkPipeline pipe;
	VkCommandBuffer cmd;
	CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = load_spv(name),
			   .pName = "main" },
		.layout = pl }, NULL, &pipe));
	cmd = begin();
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
	if (push_size)
		vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size, push);
	vkCmdDispatch(cmd, gx, 1, 1);
	run(cmd);
}

static VkSampler nearest(void)
{
	VkSampler s;
	CHECK(vkCreateSampler(dev, &(VkSamplerCreateInfo){
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE }, NULL, &s));
	return s;
}

/* a render pass with ncol colour attachments (RGBA8, samples) and maybe a
 * D32 depth attachment, all cleared; colours end in col_final
 */
static VkRenderPass make_rp(int ncol, VkSampleCountFlagBits samples, int depth,
			    VkImageLayout col_final)
{
	VkAttachmentDescription a[3];
	VkAttachmentReference cr[2], dr;
	VkRenderPass rp;
	int n = 0;
	for (int i = 0; i < ncol; i++) {
		a[n] = (VkAttachmentDescription){
			.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = samples,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = col_final };
		cr[i] = (VkAttachmentReference){ n++, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	}
	if (depth) {
		a[n] = (VkAttachmentDescription){
			.format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
		dr = (VkAttachmentReference){ n++, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	}
	CHECK(vkCreateRenderPass(dev, &(VkRenderPassCreateInfo){
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = n,
		.pAttachments = a, .subpassCount = 1, .pSubpasses = &(VkSubpassDescription){
			.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
			.colorAttachmentCount = ncol, .pColorAttachments = cr,
			.pDepthStencilAttachment = depth ? &dr : NULL } }, NULL, &rp));
	return rp;
}

struct gfx {
	const char *vs, *fs;		/* fs NULL: rasterizer discard */
	VkPrimitiveTopology topo;
	VkSampleCountFlagBits samples;
	int sample_shading;
	int ncol;
	const VkPipelineColorBlendAttachmentState *blend;
	int depth;
	float bias_const, bias_clamp;
	int vertex_attr;		/* one vec4 attribute at binding 0 */
	uint32_t w, h;
};

static VkPipeline make_gfx(const struct gfx *g, VkRenderPass rp)
{
	VkPipeline pipe;
	VkPipelineShaderStageCreateInfo st[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = load_spv(g->vs), .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .pName = "main" } };
	if (g->fs)
		st[1].module = load_spv(g->fs);
	CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &(VkGraphicsPipelineCreateInfo){
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = g->fs ? 2 : 1, .pStages = st,
		.pVertexInputState = &(VkPipelineVertexInputStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			.vertexBindingDescriptionCount = g->vertex_attr ? 1 : 0,
			.pVertexBindingDescriptions = &(VkVertexInputBindingDescription){
				0, 16, VK_VERTEX_INPUT_RATE_VERTEX },
			.vertexAttributeDescriptionCount = g->vertex_attr ? 1 : 0,
			.pVertexAttributeDescriptions = &(VkVertexInputAttributeDescription){
				0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 } },
		.pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = g->topo },
		.pViewportState = &(VkPipelineViewportStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1,
			.pViewports = &(VkViewport){ 0, 0, g->w, g->h, 0, 1 },
			.scissorCount = 1, .pScissors = &(VkRect2D){ { 0, 0 }, { g->w, g->h } } },
		.pRasterizationState = &(VkPipelineRasterizationStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.rasterizerDiscardEnable = g->fs == NULL,
			.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
			.depthBiasEnable = g->bias_const != 0.0f,
			.depthBiasConstantFactor = g->bias_const,
			.depthBiasClamp = g->bias_clamp, .lineWidth = 1.0f },
		.pMultisampleState = &(VkPipelineMultisampleStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			.rasterizationSamples = g->samples,
			.sampleShadingEnable = g->sample_shading, .minSampleShading = 1.0f },
		.pDepthStencilState = g->depth ? &(VkPipelineDepthStencilStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
			.depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
			.depthCompareOp = VK_COMPARE_OP_ALWAYS } : NULL,
		.pColorBlendState = &(VkPipelineColorBlendStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = g->ncol, .pAttachments = g->blend },
		.layout = pl, .renderPass = rp }, NULL, &pipe));
	return pipe;
}

static VkFramebuffer make_fb(VkRenderPass rp, int n, const VkImageView *views,
			     uint32_t w, uint32_t h)
{
	VkFramebuffer fb;
	CHECK(vkCreateFramebuffer(dev, &(VkFramebufferCreateInfo){
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
		.attachmentCount = n, .pAttachments = views, .width = w, .height = h,
		.layers = 1 }, NULL, &fb));
	return fb;
}

static void begin_rp(VkCommandBuffer cmd, VkRenderPass rp, VkFramebuffer fb,
		     uint32_t w, uint32_t h, int nclear, const VkClearValue *cv)
{
	vkCmdBeginRenderPass(cmd, &(VkRenderPassBeginInfo){
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
		.framebuffer = fb, .renderArea = { { 0, 0 }, { w, h } },
		.clearValueCount = nclear, .pClearValues = cv }, VK_SUBPASS_CONTENTS_INLINE);
}

static uint32_t colour(int c)
{
	return ((c * 37 + 16) & 0xff) | (((200 - c * 5) & 0xff) << 8) | ((c & 0xff) << 16) |
	       0xff000000u;
}

static int bad;
static const char *devname;

static void verdict(const char *what, int b0)
{
	printf("  %s: %s\n", what, bad == b0 ? "correct" : "WRONG");
}

/* Z1 */
static const struct { VkFormat f; const char *name; int bytes; float tol; } bcf[NBC] = {
	{ VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1 RGBA", 8, 3.0f / 255 },
	{ VK_FORMAT_BC1_RGB_SRGB_BLOCK, "BC1 sRGB", 8, 3.0f / 255 },
	{ VK_FORMAT_BC2_UNORM_BLOCK, "BC2", 16, 3.0f / 255 },
	{ VK_FORMAT_BC3_UNORM_BLOCK, "BC3", 16, 3.0f / 255 },
	{ VK_FORMAT_BC4_UNORM_BLOCK, "BC4", 8, 2.0f / 255 },
	{ VK_FORMAT_BC4_SNORM_BLOCK, "BC4 snorm", 8, 2.0f / 127 },
	{ VK_FORMAT_BC5_UNORM_BLOCK, "BC5", 16, 2.0f / 255 },
	{ VK_FORMAT_BC5_SNORM_BLOCK, "BC5 snorm", 16, 2.0f / 127 },
	{ VK_FORMAT_BC6H_UFLOAT_BLOCK, "BC6H ufloat", 16, 0.01f },
	{ VK_FORMAT_BC6H_SFLOAT_BLOCK, "BC6H sfloat", 16, 0.01f },
	{ VK_FORMAT_BC7_UNORM_BLOCK, "BC7", 16, 2.0f / 255 },
	{ VK_FORMAT_BC7_SRGB_BLOCK, "BC7 sRGB", 16, 2.0f / 255 },
};

static void z1(int slot)
{
	VkSampler s = nearest();
	VkBuffer src, out;
	uint8_t *sm;
	float *om;
	uint32_t rng = 12345;
	make_set(2, (VkDescriptorType[]){ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },
		 VK_SHADER_STAGE_COMPUTE_BIT, 0, 0);
	sm = make_buffer(&src, 64);
	om = make_buffer(&out, 64 * 16);
	for (int f = 0; f < NBC; f++) {
		VkFormatProperties fp;
		VkImage img;
		VkCommandBuffer cmd;
		vkGetPhysicalDeviceFormatProperties(pdev, bcf[f].f, &fp);
		if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
			bcok[slot][f] = 0;
			continue;
		}
		for (int i = 0; i < 64; i++) {
			rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
			sm[i] = rng >> 24;
		}
		img = make_image(bcf[f].f, 8, 8, 1, VK_SAMPLE_COUNT_1_BIT,
				 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0);
		cmd = begin();
		layout(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED,
		       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		vkCmdCopyBufferToImage(cmd, src, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
			&(VkBufferImageCopy){ .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
					      .imageExtent = { 8, 8, 1 } });
		layout(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		run(cmd);
		set_img(0, s, make_view(img, VK_IMAGE_VIEW_TYPE_2D, bcf[f].f,
					VK_IMAGE_ASPECT_COLOR_BIT, 1));
		set_buf(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, out, 0, VK_WHOLE_SIZE);
		memset(om, 0x55, 64 * 16);
		compute("bc.comp", 1, NULL, 0);
		memcpy(bcres[slot][f], om, sizeof(bcres[slot][f]));
		bcok[slot][f] = 1;
	}
}

static void z1_compare(void)
{
	printf("Z1 BC formats, NVK against lavapipe (64 texels each):\n");
	for (int f = 0; f < NBC; f++) {
		int nbad = 0;
		float maxd = 0;
		if (!bcok[0][f] || !bcok[1][f]) {
			printf("  %-12s not compared: sampled-image support NVK %d, lavapipe %d\n",
			       bcf[f].name, bcok[0][f], bcok[1][f]);
			if (!bcok[0][f])
				bad++;
			continue;
		}
		for (int i = 0; i < 256; i++) {
			float a = bcres[0][f][i], b = bcres[1][f][i];
			float d = fabsf(a - b), tol = bcf[f].tol;
			if (f == 8 || f == 9)
				tol = 0.01f * fmaxf(fabsf(a), fabsf(b)) + 1e-3f;
			if (isnan(a) && isnan(b))
				continue;
			if (!(d <= tol))
				nbad++;
			if (d > maxd)
				maxd = d;
		}
		printf("  %-12s %s (largest difference %g, %d of 256 values outside tolerance)\n",
		       bcf[f].name, nbad ? "WRONG" : "matches", maxd, nbad);
		if (nbad && getenv("VKDAWN_DUMP")) {
			for (int i = 0; i < 64; i++)
				printf("    texel %2d: nvk %.4f %.4f %.4f %.4f  lvp %.4f %.4f %.4f %.4f\n", i,
				       bcres[0][f][i * 4], bcres[0][f][i * 4 + 1], bcres[0][f][i * 4 + 2],
				       bcres[0][f][i * 4 + 3], bcres[1][f][i * 4], bcres[1][f][i * 4 + 1],
				       bcres[1][f][i * 4 + 2], bcres[1][f][i * 4 + 3]);
		}
		bad += nbad != 0;
	}
}

/* Z2 */
static void z2(void)
{
	VkBuffer big, res, vb, vout;
	uint32_t *bm, *rm;
	float *vm, *vo;
	int b0, inr = 0, oob_zero = 0, oob_in = 0, oob_buf = 0, oob_other = 0;
	int u_inr = 0, u_zero = 0, u_in = 0, u_buf = 0, u_other = 0, w_in = 0, w_out = 0;
	uint32_t zero = 0;

	printf("Z2 robustBufferAccess:\n");
	make_set(4, (VkDescriptorType[]){ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
					  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },
		 VK_SHADER_STAGE_COMPUTE_BIT, 4, VK_SHADER_STAGE_COMPUTE_BIT);
	bm = make_buffer(&big, 4096);
	for (int i = 0; i < 1024; i++)
		bm[i] = 0xdeadbeef;
	for (int i = 0; i < 16; i++) {
		bm[256 + i] = 100 + i;	/* storage binding at byte 1024 */
		bm[768 + i] = 200 + i;	/* uniform binding at byte 3072 */
	}
	rm = make_buffer(&res, 256);
	set_buf(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, big, 1024, 64);
	set_buf(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, big, 2048, 64);
	set_buf(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, big, 3072, 64);
	set_buf(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, res, 0, VK_WHOLE_SIZE);
	compute("robust.comp", 1, &zero, 4);
	for (int i = 0; i < 32; i++) {
		uint32_t v = rm[i], u = rm[32 + i];
		if (i < 16) {
			inr += v == 100u + i;
			u_inr += u == 200u + i;
		} else {
			/* allowed: zero, or any value from the memory bound to the
			 * buffer (the bindings' words, the 0xdeadbeef around them)
			 */
			if (v == 0) oob_zero++; else if (v >= 100 && v < 116) oob_in++;
			else if (v == 0xdeadbeef || (v >= 200 && v < 216)) oob_buf++; else oob_other++;
			if (u == 0) u_zero++; else if (u >= 200 && u < 216) u_in++;
			else if (u == 0xdeadbeef || (u >= 100 && u < 116)) u_buf++; else u_other++;
		}
	}
	for (int i = 0; i < 1024; i++) {
		int inside = i >= 512 && i < 528;
		if (inside)
			w_in += bm[i] >= 500 && bm[i] < 532;
		else if (i < 256 || (i >= 272 && i < 768) || i >= 784)
			w_out += bm[i] != 0xdeadbeef;
	}
	b0 = bad;
	bad += inr != 16 || oob_other != 0;
	printf("  storage reads: %d of 16 in range correct; past the end: %d zero, %d from "
	       "the binding, %d from elsewhere in the buffer, %d other\n", inr, oob_zero,
	       oob_in, oob_buf, oob_other);
	verdict("storage reads", b0);
	b0 = bad;
	bad += u_inr != 16 || u_other != 0;
	printf("  uniform reads: %d of 16 in range correct; past the end: %d zero, %d from "
	       "the binding, %d from elsewhere in the buffer, %d other\n", u_inr, u_zero,
	       u_in, u_buf, u_other);
	if (u_other)
		printf("  uniform reads past the end, first four: %08x %08x %08x %08x\n",
		       rm[48], rm[49], rm[50], rm[51]);
	verdict("uniform reads", b0);
	b0 = bad;
	bad += w_in != 16 || w_out != 0;
	printf("  storage writes: %d of 16 words of the binding written; %d words outside "
	       "it changed\n", w_in, w_out);
	verdict("storage writes", b0);

	/* vertex attributes: a 4-vertex buffer at the start of a larger
	 * allocation whose rest holds 12345.0, 8 vertices drawn
	 */
	make_set(1, (VkDescriptorType[]){ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },
		 VK_SHADER_STAGE_VERTEX_BIT, 0, 0);
	vm = make_buffer2(&vb, 64, 4096, 0);
	for (int i = 0; i < 1024; i++)
		vm[i] = i < 16 ? 1.0f + i : 12345.0f;
	vo = make_buffer(&vout, 8 * 16);
	set_buf(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vout, 0, VK_WHOLE_SIZE);
	{
		struct gfx g = { .vs = "vrobust.vert", .topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
				 .samples = VK_SAMPLE_COUNT_1_BIT, .vertex_attr = 1, .w = 1, .h = 1 };
		VkRenderPass rp = make_rp(0, VK_SAMPLE_COUNT_1_BIT, 0, 0);
		VkPipeline pipe = make_gfx(&g, rp);
		VkFramebuffer fb = make_fb(rp, 0, NULL, 1, 1);
		VkCommandBuffer cmd = begin();
		VkDeviceSize off = 0;
		int vin = 0, vzero = 0, vfrom = 0, vother = 0;
		begin_rp(cmd, rp, fb, 1, 1, 0, NULL);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, NULL);
		vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
		vkCmdDraw(cmd, 8, 1, 0, 0);
		vkCmdEndRenderPass(cmd);
		run(cmd);
		for (int v = 0; v < 8; v++) {
			float *a = &vo[v * 4];
			if (v < 4) {
				vin += a[0] == 1.0f + v * 4 && a[3] == 4.0f + v * 4;
				continue;
			}
			if (a[0] == 0 && a[1] == 0 && a[2] == 0 && (a[3] == 0 || a[3] == 1))
				vzero++;
			else if (a[0] >= 1.0f && a[0] <= 16.0f)
				vfrom++;
			else
				vother++;
		}
		b0 = bad;
		bad += vin != 4 || vother != 0;
		printf("  vertex attributes: %d of 4 in range correct; past the end: %d zero, "
		       "%d from the buffer, %d other (vertex 4: %g %g %g %g)\n", vin, vzero, vfrom,
		       vother, vo[16], vo[17], vo[18], vo[19]);
		verdict("vertex attributes", b0);
	}
}

/* Z3 */
static void z3(void)
{
	VkImage img;
	VkCommandBuffer cmd;
	VkBuffer out;
	uint32_t *om;
	int b0 = bad;
	printf("Z3 imageCubeArray:\n");
	img = make_image(VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 12, VK_SAMPLE_COUNT_1_BIT,
			 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			 VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
	cmd = begin();
	layout(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, 12, VK_IMAGE_LAYOUT_UNDEFINED,
	       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	for (int l = 0; l < 12; l++) {
		uint32_t v = colour(40 + l);
		VkClearColorValue cc = { .float32 = { (v & 0xff) / 255.0f, ((v >> 8) & 0xff) / 255.0f,
						     ((v >> 16) & 0xff) / 255.0f, 1.0f } };
		vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1,
			&(VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, l, 1 });
	}
	layout(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, 12, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	run(cmd);
	make_set(2, (VkDescriptorType[]){ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },
		 VK_SHADER_STAGE_COMPUTE_BIT, 0, 0);
	om = make_buffer(&out, 64);
	set_img(0, nearest(), make_view(img, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY,
					VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 12));
	set_buf(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, out, 0, VK_WHOLE_SIZE);
	compute("cubearr.comp", 1, NULL, 0);
	for (int k = 0; k < 12; k++) {
		if (om[k] != colour(40 + k)) {
			printf("  cube %d face %d: %08x, expect %08x\n", k / 6, k % 6, om[k],
			       colour(40 + k));
			bad++;
		}
	}
	verdict("12 faces of 2 cubes", b0);
}

/* Z4: 8x8 4x image drawn with frag, then every sample read back */
static void z4_draw(const char *frag, int sample_shading, uint32_t *samples)
{
	VkImage img;
	VkImageView view;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkPipeline pipe;
	VkCommandBuffer cmd;
	VkBuffer out;
	uint32_t *om;
	float pc[2] = { 0.5f, 8.0f };
	VkPipelineColorBlendAttachmentState bl = { .colorWriteMask = 0xf };
	struct gfx g = { .vs = "full.vert", .fs = frag,
			 .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
			 .samples = VK_SAMPLE_COUNT_4_BIT, .sample_shading = sample_shading,
			 .ncol = 1, .blend = &bl, .w = 8, .h = 8 };

	img = make_image(VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, VK_SAMPLE_COUNT_4_BIT,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0);
	view = make_view(img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
			 VK_IMAGE_ASPECT_COLOR_BIT, 1);
	make_set(0, NULL, 0, 8, VK_SHADER_STAGE_VERTEX_BIT);
	rp = make_rp(1, VK_SAMPLE_COUNT_4_BIT, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	pipe = make_gfx(&g, rp);
	fb = make_fb(rp, 1, &view, 8, 8);
	cmd = begin();
	begin_rp(cmd, rp, fb, 8, 8, 1, &(VkClearValue){ .color = { .float32 = { 0, 0, 0, 1 } } });
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, pc);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
	run(cmd);

	make_set(2, (VkDescriptorType[]){ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },
		 VK_SHADER_STAGE_COMPUTE_BIT, 0, 0);
	om = make_buffer(&out, 256 * 4);
	set_img(0, nearest(), view);
	set_buf(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, out, 0, VK_WHOLE_SIZE);
	compute("msfetch.comp", 1, NULL, 0);
	memcpy(samples, om, 256 * 4);
}

/* fragment shader invocations of one full-target triangle, 8x8 at 4x */
static int z4_count(int sample_shading)
{
	VkImage img;
	VkImageView view;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkPipeline pipe;
	VkCommandBuffer cmd;
	VkBuffer cnt;
	uint32_t *cm;
	float pc[2] = { 0.5f, 8.0f };
	VkPipelineColorBlendAttachmentState bl = { .colorWriteMask = 0xf };
	struct gfx g = { .vs = "full.vert", .fs = "count.frag",
			 .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
			 .samples = VK_SAMPLE_COUNT_4_BIT, .sample_shading = sample_shading,
			 .ncol = 1, .blend = &bl, .w = 8, .h = 8 };
	img = make_image(VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, VK_SAMPLE_COUNT_4_BIT,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0);
	view = make_view(img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
			 VK_IMAGE_ASPECT_COLOR_BIT, 1);
	make_set(1, (VkDescriptorType[]){ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },
		 VK_SHADER_STAGE_FRAGMENT_BIT, 8, VK_SHADER_STAGE_VERTEX_BIT);
	cm = make_buffer(&cnt, 16);
	cm[0] = 0;
	set_buf(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cnt, 0, VK_WHOLE_SIZE);
	rp = make_rp(1, VK_SAMPLE_COUNT_4_BIT, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	pipe = make_gfx(&g, rp);
	fb = make_fb(rp, 1, &view, 8, 8);
	cmd = begin();
	begin_rp(cmd, rp, fb, 8, 8, 1, &(VkClearValue){ .color = { .float32 = { 0, 0, 0, 1 } } });
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, NULL);
	vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, pc);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
	run(cmd);
	return cm[0];
}

static void z4(void)
{
	uint32_t s[256];
	int b0, distinct = 0, std = 0;
	printf("Z4 sampleRateShading:\n");
	z4_draw("sid.frag", 0, s);
	b0 = bad;
	for (int p = 0; p < 64; p++)
		for (int k = 0; k < 4; k++) {
			int want = (int)lrintf((k * 0.25f + 0.1f) * 255.0f);
			int got = s[p * 4 + k] & 0xff;
			if (abs(got - want) > 1) {
				if (bad - b0 < 4)
					printf("  pixel %d sample %d: red %d, expect %d\n", p, k, got, want);
				bad++;
			}
		}
	verdict("a: gl_SampleID in every sample of 64 pixels", b0);

	b0 = bad;
	distinct = z4_count(0);
	std = z4_count(1);
	/* without sample shading an implementation may still run more than one
	 * invocation per pixel; with minSampleShading 1.0 it must run at least one
	 * per sample
	 */
	bad += std < 256;
	printf("  b: fragment shader invocations over 64 pixels at 4x: %d without sample "
	       "shading, %d with minSampleShading 1.0 (expect at least 256)\n",
	       distinct, std);
	verdict("b: minSampleShading 1.0", b0);
}

/* Z5 */
static void z5(void)
{
	VkImage img[2];
	VkImageView view[2];
	VkRenderPass rp;
	VkFramebuffer fb;
	VkPipeline pipe;
	VkCommandBuffer cmd;
	VkBuffer out;
	uint8_t *om;
	float pc[2] = { 0.5f, 4.0f };
	int b0 = bad;
	VkPipelineColorBlendAttachmentState bl[2] = {
		{ .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
		  .dstColorBlendFactor = VK_BLEND_FACTOR_ONE, .colorBlendOp = VK_BLEND_OP_ADD,
		  .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
		  .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf },
		{ .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
		  .dstColorBlendFactor = VK_BLEND_FACTOR_ONE, .colorBlendOp = VK_BLEND_OP_MIN,
		  .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
		  .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf } };
	struct gfx g = { .vs = "full.vert", .fs = "two.frag",
			 .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
			 .samples = VK_SAMPLE_COUNT_1_BIT, .ncol = 2, .blend = bl, .w = 4, .h = 4 };
	VkClearValue cv[2] = { { .color = { .float32 = { 0.2f, 0.2f, 0.2f, 1 } } },
			       { .color = { .float32 = { 0.2f, 0.2f, 0.2f, 1 } } } };

	printf("Z5 independentBlend:\n");
	for (int i = 0; i < 2; i++) {
		img[i] = make_image(VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, VK_SAMPLE_COUNT_1_BIT,
				    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
				    VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0);
		view[i] = make_view(img[i], VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
				    VK_IMAGE_ASPECT_COLOR_BIT, 1);
	}
	make_set(0, NULL, 0, 8, VK_SHADER_STAGE_VERTEX_BIT);
	rp = make_rp(2, VK_SAMPLE_COUNT_1_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	pipe = make_gfx(&g, rp);
	fb = make_fb(rp, 2, view, 4, 4);
	om = make_buffer(&out, 2 * 64);
	cmd = begin();
	begin_rp(cmd, rp, fb, 4, 4, 2, cv);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, pc);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
	for (int i = 0; i < 2; i++)
		vkCmdCopyImageToBuffer(cmd, img[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out, 1,
			&(VkBufferImageCopy){ .bufferOffset = i * 64,
				.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
				.imageExtent = { 4, 4, 1 } });
	run(cmd);
	/* 0.2 + 0.4 = 0.6 -> 153; min(0.4, 0.2) = 0.2 -> 51 */
	for (int p = 0; p < 16; p++) {
		if (abs(om[p * 4] - 153) > 1 || abs(om[64 + p * 4] - 51) > 1) {
			if (bad - b0 < 2)
				printf("  pixel %d: attachment 0 %d (expect 153), attachment 1 %d "
				       "(expect 51)\n", p, om[p * 4], om[64 + p * 4]);
			bad++;
		}
	}
	printf("  pixel 0: attachment 0 %d, attachment 1 %d\n", om[0], om[64]);
	verdict("ADD and MIN on two attachments", b0);
}

/* Z6 */
static float z6_draw(float bias, float clamp)
{
	VkImage img;
	VkImageView view;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkPipeline pipe;
	VkCommandBuffer cmd;
	VkBuffer out;
	float *om;
	float pc[2] = { 0.5f, 4.0f };
	struct gfx g = { .vs = "full.vert", .fs = "none.frag",
			 .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
			 .samples = VK_SAMPLE_COUNT_1_BIT, .depth = 1, .bias_const = bias,
			 .bias_clamp = clamp, .w = 4, .h = 4 };
	img = make_image(VK_FORMAT_D32_SFLOAT, 4, 4, 1, VK_SAMPLE_COUNT_1_BIT,
			 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
			 VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0);
	view = make_view(img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_D32_SFLOAT,
			 VK_IMAGE_ASPECT_DEPTH_BIT, 1);
	make_set(0, NULL, 0, 8, VK_SHADER_STAGE_VERTEX_BIT);
	rp = make_rp(0, VK_SAMPLE_COUNT_1_BIT, 1, 0);
	pipe = make_gfx(&g, rp);
	fb = make_fb(rp, 1, &view, 4, 4);
	om = make_buffer(&out, 64);
	cmd = begin();
	begin_rp(cmd, rp, fb, 4, 4, 1,
		 &(VkClearValue){ .depthStencil = { 1.0f, 0 } });
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, pc);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
	vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out, 1,
		&(VkBufferImageCopy){ .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
				      .imageExtent = { 4, 4, 1 } });
	run(cmd);
	return om[5];
}

static void z6(void)
{
	float none = z6_draw(0.0f, 0.0f);
	float up = z6_draw(1.0e6f, 0.01f);
	float down = z6_draw(-1.0e6f, -0.01f);
	int b0 = bad;
	printf("Z6 depthBiasClamp:\n");
	bad += fabsf(none - 0.5f) > 1e-6f;
	bad += fabsf(up - 0.51f) > 1e-5f;
	bad += fabsf(down - 0.49f) > 1e-5f;
	printf("  depth without bias %.7f (expect 0.5), +1e6 clamped to +0.01: %.7f (expect "
	       "0.51), -1e6 clamped to -0.01: %.7f (expect 0.49)\n", none, up, down);
	verdict("bias clamped both ways", b0);
}

/* Z9: push constants as a shader stage sees them; frag NULL = the vertex
 * shader stores them (one point, rasterizer off), else the fragment shader
 * of a 1x1 triangle; stages = the push constant range's stages
 */
static void z9_run(const char *what, int frag, VkShaderStageFlags stages)
{
	VkBuffer out;
	float *om, pc[2] = { 0.5f, 8.0f };
	VkPipelineColorBlendAttachmentState bl = { .colorWriteMask = 0xf };
	struct gfx g = { .vs = frag ? "plain.vert" : "pcv.vert", .fs = frag ? "pcf.frag" : NULL,
			 .topo = frag ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST :
					VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
			 .samples = VK_SAMPLE_COUNT_1_BIT, .ncol = frag, .blend = &bl,
			 .w = 1, .h = 1 };
	VkImage img;
	VkImageView view;
	VkRenderPass rp;
	VkPipeline pipe;
	VkFramebuffer fb;
	VkCommandBuffer cmd;
	int b0 = bad;
	make_set(1, (VkDescriptorType[]){ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },
		 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 8, stages);
	om = make_buffer(&out, 16);
	set_buf(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, out, 0, VK_WHOLE_SIZE);
	rp = make_rp(frag, VK_SAMPLE_COUNT_1_BIT, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	pipe = make_gfx(&g, rp);
	if (frag) {
		img = make_image(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT,
				 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0);
		view = make_view(img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
				 VK_IMAGE_ASPECT_COLOR_BIT, 1);
		fb = make_fb(rp, 1, &view, 1, 1);
	} else {
		fb = make_fb(rp, 0, NULL, 1, 1);
	}
	cmd = begin();
	begin_rp(cmd, rp, fb, 1, 1, frag,
		 &(VkClearValue){ .color = { .float32 = { 0, 0, 0, 1 } } });
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, NULL);
	vkCmdPushConstants(cmd, pl, stages, 0, 8, pc);
	vkCmdDraw(cmd, frag ? 3 : 1, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
	run(cmd);
	bad += om[0] != 0.5f || om[1] != 8.0f;
	printf("  %s: %g %g (expect 0.5 8)\n", what, om[0], om[1]);
	verdict(what, b0);
}

/* Z0 diagnosis: one trivial dispatch */
static void z0(void)
{
	VkBuffer out;
	uint32_t *om;
	make_set(1, (VkDescriptorType[]){ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },
		 VK_SHADER_STAGE_COMPUTE_BIT, 0, 0);
	om = make_buffer(&out, 16);
	set_buf(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, out, 0, VK_WHOLE_SIZE);
	compute(getenv("VKDAWN_Z0NOOP") ? "noop.comp" : "triv.comp", 1, NULL, 0);
	printf("Z0 trivial dispatch wrote %u (expect 42)\n", om[0]);
}

/* Z9 diagnosis: which push constant words a vertex shader sees */
static void z9_map(void)
{
	VkBuffer out;
	uint32_t *om, pc[32];
	struct gfx g = { .vs = "pcmap.vert", .topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
			 .samples = VK_SAMPLE_COUNT_1_BIT, .w = 1, .h = 1 };
	VkRenderPass rp;
	VkPipeline pipe;
	VkFramebuffer fb;
	VkCommandBuffer cmd;
	for (int i = 0; i < 32; i++)
		pc[i] = 0x1000 + i;
	make_set(1, (VkDescriptorType[]){ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },
		 VK_SHADER_STAGE_VERTEX_BIT, 128, VK_SHADER_STAGE_VERTEX_BIT);
	om = make_buffer(&out, 256);
	set_buf(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, out, 0, VK_WHOLE_SIZE);
	rp = make_rp(0, VK_SAMPLE_COUNT_1_BIT, 0, 0);
	pipe = make_gfx(&g, rp);
	fb = make_fb(rp, 0, NULL, 1, 1);
	cmd = begin();
	if (getenv("VKDAWN_SAMECB")) {
		/* a dispatch in the same command buffer, before the draw */
		VkPipeline cp;
		VkDescriptorSetLayout cdsl;
		VkPipelineLayout cpl;
		VkDescriptorPool dp;
		VkDescriptorSet cset;
		VkBuffer cout;
		CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1,
			.pBindings = &(VkDescriptorSetLayoutBinding){ 0,
				VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } },
			NULL, &cdsl));
		CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1, .pSetLayouts = &cdsl }, NULL, &cpl));
		CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
			.poolSizeCount = 1, .pPoolSizes = &(VkDescriptorPoolSize){
				VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } }, NULL, &dp));
		CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &cdsl }, &cset));
		make_buffer(&cout, 16);
		vkUpdateDescriptorSets(dev, 1, &(VkWriteDescriptorSet){
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = cset,
			.dstBinding = 0, .descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &(VkDescriptorBufferInfo){ cout, 0, VK_WHOLE_SIZE } }, 0, NULL);
		CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = load_spv("triv.comp"),
				   .pName = "main" }, .layout = cpl }, NULL, &cp));
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cp);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cpl, 0, 1, &cset, 0, NULL);
		vkCmdDispatch(cmd, 1, 1, 1);
	}
	begin_rp(cmd, rp, fb, 1, 1, 0, NULL);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, NULL);
	vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 128, pc);
	vkCmdDraw(cmd, 1, 1, 0, 7);
	vkCmdEndRenderPass(cmd);
	run(cmd);
	printf("  push words seen:");
	for (int i = 0; i < 32; i++)
		printf(" %x", om[i]);
	printf("; gl_InstanceIndex %u (expect 7)\n", om[32]);
}

static void z9(void)
{
	if (getenv("VKDAWN_PCMAP"))
		z9_map();
	printf("Z9 push constants in graphics stages:\n");
	z9_run("vertex shader, vertex range", 0, VK_SHADER_STAGE_VERTEX_BIT);
	z9_run("vertex shader, vertex+fragment range", 0,
	       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	z9_run("fragment shader, fragment range", 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	z9_run("fragment shader, vertex+fragment range", 1,
	       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
}

/* Z7 */
static void z7(void)
{
	static const uint32_t idx[4] = { 0x01000000u, 0x01000001u, 0x0fffffffu, 0xfffffffeu };
	VkBuffer ib, out;
	uint32_t *im, *om;
	int b0 = bad, found = 0;
	struct gfx g = { .vs = "idx.vert", .topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
			 .samples = VK_SAMPLE_COUNT_1_BIT, .w = 1, .h = 1 };
	VkRenderPass rp;
	VkPipeline pipe;
	VkFramebuffer fb;
	VkCommandBuffer cmd;
	printf("Z7 fullDrawIndexUint32:\n");
	make_set(1, (VkDescriptorType[]){ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },
		 VK_SHADER_STAGE_VERTEX_BIT, 0, 0);
	im = make_buffer(&ib, 16);
	memcpy(im, idx, 16);
	om = make_buffer(&out, 64);
	om[0] = 0;
	set_buf(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, out, 0, VK_WHOLE_SIZE);
	rp = make_rp(0, VK_SAMPLE_COUNT_1_BIT, 0, 0);
	pipe = make_gfx(&g, rp);
	fb = make_fb(rp, 0, NULL, 1, 1);
	cmd = begin();
	begin_rp(cmd, rp, fb, 1, 1, 0, NULL);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, NULL);
	vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexed(cmd, 4, 1, 0, 0, 0);
	vkCmdEndRenderPass(cmd);
	run(cmd);
	for (int i = 0; i < 4; i++)
		for (uint32_t k = 0; k < om[0] && k < 8; k++)
			if (om[1 + k] == idx[i]) {
				found++;
				break;
			}
	bad += om[0] != 4 || found != 4;
	printf("  %u vertices ran; indices seen: %08x %08x %08x %08x\n", om[0], om[1], om[2],
	       om[3], om[4]);
	verdict("four indices above 2^24", b0);
}

/* Z8 */
static void z8(void)
{
	VkPhysicalDeviceProperties p;
	const VkPhysicalDeviceLimits *l = &p.limits;
	int nb = 0;
	vkGetPhysicalDeviceProperties(pdev, &p);
	printf("Z8 limits against WebGPU's core minimums (claimed by the driver):\n");
#define MIN_(name, v) do { if ((uint64_t)l->name < (uint64_t)(v)) { \
	printf("  %s %llu < %llu\n", #name, (unsigned long long)l->name, (unsigned long long)(v)); nb++; } } while (0)
#define MAX_(name, v) do { if ((uint64_t)l->name > (uint64_t)(v)) { \
	printf("  %s %llu > %llu\n", #name, (unsigned long long)l->name, (unsigned long long)(v)); nb++; } } while (0)
	MIN_(maxImageDimension1D, 8192);
	MIN_(maxImageDimension2D, 8192);
	MIN_(maxImageDimension3D, 2048);
	MIN_(maxImageArrayLayers, 256);
	MIN_(maxBoundDescriptorSets, 4);
	MIN_(maxPerStageDescriptorSamplers, 16);
	MIN_(maxPerStageDescriptorSampledImages, 16);
	MIN_(maxPerStageDescriptorStorageBuffers, 8);
	MIN_(maxPerStageDescriptorStorageImages, 4);
	MIN_(maxPerStageDescriptorUniformBuffers, 12);
	MIN_(maxUniformBufferRange, 65536);
	MIN_(maxStorageBufferRange, 134217728);
	MAX_(minUniformBufferOffsetAlignment, 256);
	MAX_(minStorageBufferOffsetAlignment, 256);
	MIN_(maxVertexInputBindings, 8);
	MIN_(maxVertexInputAttributes, 16);
	MIN_(maxVertexInputBindingStride, 2048);
	MIN_(maxVertexOutputComponents, 64);
	MIN_(maxFragmentInputComponents, 64);
	MIN_(maxColorAttachments, 8);
	MIN_(maxComputeSharedMemorySize, 16384);
	MIN_(maxComputeWorkGroupInvocations, 256);
	MIN_(maxComputeWorkGroupSize[0], 256);
	MIN_(maxComputeWorkGroupSize[1], 256);
	MIN_(maxComputeWorkGroupSize[2], 64);
	MIN_(maxComputeWorkGroupCount[0], 65535);
	MIN_(maxComputeWorkGroupCount[1], 65535);
	MIN_(maxComputeWorkGroupCount[2], 65535);
	printf("  %d below WebGPU's minimum\n", nb);
	printf("  claimed per stage: %u samplers, %u sampled images, %u storage images, "
	       "%u uniform buffers, %u storage buffers; Fermi binds 16 samplers and 32 "
	       "textures per stage\n", l->maxPerStageDescriptorSamplers,
	       l->maxPerStageDescriptorSampledImages, l->maxPerStageDescriptorStorageImages,
	       l->maxPerStageDescriptorUniformBuffers, l->maxPerStageDescriptorStorageBuffers);
	printf("  api %u.%u.%u, maxDrawIndexedIndexValue %u\n", VK_API_VERSION_MAJOR(p.apiVersion),
	       VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
	       l->maxDrawIndexedIndexValue);
	bad += nb != 0;
}

int main(void)
{
	VkInstance inst;
	uint32_t n = 8;
	VkPhysicalDevice pd[8];
	int total = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	CHECK(vkCreateInstance(&(VkInstanceCreateInfo){
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &(VkApplicationInfo){
			.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
			.apiVersion = VK_API_VERSION_1_1 } }, NULL, &inst));
	CHECK(vkEnumeratePhysicalDevices(inst, &n, pd));
	for (int pass = 0; pass < 2; pass++) {
		for (uint32_t d = 0; d < n; d++) {
			VkPhysicalDeviceProperties props;
			VkPhysicalDeviceFeatures f;
			int slot;
			vkGetPhysicalDeviceProperties(pd[d], &props);
			slot = strstr(props.deviceName, "NVK") ? 0 :
			       strstr(props.deviceName, "llvmpipe") ? 1 : -1;
			if (slot != pass)
				continue;
			pdev = pd[d];
			devname = slot ? "lavapipe" : "NVK";
			vkGetPhysicalDeviceFeatures(pdev, &f);
			printf("=== %s: %s\n", devname, props.deviceName);
			printf("claimed: BC %d robustBufferAccess %d imageCubeArray %d "
			       "sampleRateShading %d independentBlend %d depthBiasClamp %d "
			       "fullDrawIndexUint32 %d fragmentStoresAndAtomics %d "
			       "vertexPipelineStoresAndAtomics %d\n", f.textureCompressionBC,
			       f.robustBufferAccess, f.imageCubeArray, f.sampleRateShading,
			       f.independentBlend, f.depthBiasClamp, f.fullDrawIndexUint32,
			       f.fragmentStoresAndAtomics, f.vertexPipelineStoresAndAtomics);
			CHECK(vkCreateDevice(pdev, &(VkDeviceCreateInfo){
				.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
				.queueCreateInfoCount = 1,
				.pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
					.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
					.queueCount = 1, .pQueuePriorities = (float[]){ 1.0f } },
				.pEnabledFeatures = &(VkPhysicalDeviceFeatures){
					.textureCompressionBC = f.textureCompressionBC,
					.robustBufferAccess = f.robustBufferAccess,
					.imageCubeArray = f.imageCubeArray,
					.sampleRateShading = f.sampleRateShading,
					.independentBlend = f.independentBlend,
					.depthBiasClamp = f.depthBiasClamp,
					.fullDrawIndexUint32 = f.fullDrawIndexUint32,
					.fragmentStoresAndAtomics = f.fragmentStoresAndAtomics,
					.vertexPipelineStoresAndAtomics =
						f.vertexPipelineStoresAndAtomics } }, NULL, &dev));
			vkGetDeviceQueue(dev, 0, 0, &queue);
			CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){
				.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }, NULL, &pool));
			have[slot] = 1;
			bad = 0;
			/* VKDAWN_ONLY=<list of case numbers, e.g. "19"> runs only those */
#define RUN(k, call) do { const char *o_ = getenv("VKDAWN_ONLY"); \
	if (!o_ || strchr(o_, '0' + (k))) { call; } } while (0)
			if (getenv("VKDAWN_ONLY") && strchr(getenv("VKDAWN_ONLY"), '0'))
				z0();
			RUN(1, z1(slot));
			RUN(2, z2());
			RUN(3, z3());
			RUN(4, z4());
			RUN(5, z5());
			RUN(6, z6());
			RUN(7, z7());
			RUN(9, z9());
			if (slot == 0)
				RUN(8, z8());
			printf("%s: %d checks failed\n", devname, bad);
			if (slot == 0)
				total += bad;
			/* devices are left alive: the test leaks its objects */
		}
	}
	bad = 0;
	if (have[0] && have[1])
		z1_compare();
	else
		printf("Z1 not compared: need both NVK and lavapipe (VK_ICD_FILENAMES a:b)\n");
	total += bad;
	printf("NVK total: %d failed\n", total);
	return total != 0;
}
