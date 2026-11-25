/* Offscreen Vulkan test, step 9 (64x64 RGBA8 targets, shaders in ./gfx):
 *  H1 multiview: one draw into two layers, coloured by gl_ViewIndex.
 *  H2 4x multisampling with a resolve attachment.
 *  H3 vkCmdBlitImage, 64x64 to 32x32, nearest and linear.
 *  H4 transform feedback of three vertices (VK_EXT_transform_feedback).
 *  H5 imageAtomicAdd from 4096 invocations onto 16 texels, after
 *     vkCmdClearColorImage.
 *  H6 uniform and storage texel buffers in a compute shader.
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
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
			VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
			VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT },
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
static VkSampleCountFlagBits g_samples = VK_SAMPLE_COUNT_1_BIT;
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
			.rasterizationSamples = g_samples },
		.pColorBlendState = &(VkPipelineColorBlendStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = 1,
			.pAttachments = &(VkPipelineColorBlendAttachmentState){
				.colorWriteMask = 0xf } },
		.layout = layout, .renderPass = rp }, NULL, &pipe));
	return pipe;
}


static VkImage make_img(VkFormat fmt, uint32_t w, uint32_t h, uint32_t layers,
			VkSampleCountFlagBits samples, VkImageUsageFlags usage)
{
	VkImage img;
	VkMemoryRequirements mr;
	VkDeviceMemory mem;
	CHECK(vkCreateImage(dev, &(VkImageCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
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

static VkImageView make_view(VkImage img, VkFormat fmt, uint32_t layers)
{
	VkImageView view;
	CHECK(vkCreateImageView(dev, &(VkImageViewCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
		.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
		.format = fmt,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers } }, NULL, &view));
	return view;
}

static void to_layout(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to)
{
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1,
		&(VkImageMemoryBarrier){
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = 0x7ff,
			.oldLayout = from, .newLayout = to,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
					      VK_REMAINING_ARRAY_LAYERS } });
}

/* attachment 0 with the given sample count; an optional single-sampled
 * resolve attachment 1; an optional view mask
 */
static VkRenderPass make_rp(VkSampleCountFlagBits samples, int resolve, uint32_t view_mask)
{
	VkRenderPass r;
	VkAttachmentDescription att[2];
	VkRenderPassMultiviewCreateInfo mv = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO,
		.subpassCount = 1, .pViewMasks = &view_mask };
	int i;
	for (i = 0; i < 2; i++)
		att[i] = (VkAttachmentDescription){
			.format = VK_FORMAT_R8G8B8A8_UNORM,
			.samples = i ? VK_SAMPLE_COUNT_1_BIT : samples,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
	CHECK(vkCreateRenderPass(dev, &(VkRenderPassCreateInfo){
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.pNext = view_mask ? &mv : NULL,
		.attachmentCount = resolve ? 2 : 1, .pAttachments = att, .subpassCount = 1,
		.pSubpasses = &(VkSubpassDescription){
			.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
			.colorAttachmentCount = 1,
			.pColorAttachments = &(VkAttachmentReference){
				0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
			.pResolveAttachments = resolve ? &(VkAttachmentReference){
				1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } : NULL } }, NULL, &r));
	return r;
}

static VkFramebuffer make_fb(VkRenderPass r, VkImageView *views, uint32_t count)
{
	VkFramebuffer f;
	CHECK(vkCreateFramebuffer(dev, &(VkFramebufferCreateInfo){
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = r,
		.attachmentCount = count, .pAttachments = views,
		.width = W, .height = H, .layers = 1 }, NULL, &f));
	return f;
}

static VkCommandBuffer begin_rp(VkRenderPass r, VkFramebuffer f)
{
	VkCommandBuffer cmd = begin();
	vkCmdBeginRenderPass(cmd, &(VkRenderPassBeginInfo){
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = r,
		.framebuffer = f, .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 2,
		.pClearValues = (VkClearValue[2]){ { .color = { .float32 = { 0, 0, 0, 1 } } },
						   { .color = { .float32 = { 0, 0, 0, 1 } } } } },
		VK_SUBPASS_CONTENTS_INLINE);
	return cmd;
}

static void copy_out(VkCommandBuffer cmd, VkImage img, VkImageLayout il, uint32_t w,
		     uint32_t h, uint32_t layers)
{
	vkCmdCopyImageToBuffer(cmd, img, il, bpix, 1,
		&(VkBufferImageCopy){ .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers },
				      .imageExtent = { w, h, 1 } });
}

static int count(int first, int npix, uint8_t r, uint8_t g, uint8_t b)
{
	int i, n = 0;
	for (i = first; i < first + npix; i++)
		n += pix[i * 4] == r && pix[i * 4 + 1] == g && pix[i * 4 + 2] == b;
	return n;
}

static VkPipeline make_comp(const char *file, uint32_t nbind,
			    const VkDescriptorSetLayoutBinding *binds,
			    VkPipelineLayout *layout, VkDescriptorSetLayout *dsl)
{
	VkPipeline pipe;
	CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = nbind, .pBindings = binds }, NULL, dsl));
	CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = dsl }, NULL, layout));
	CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = load_spv(file),
			   .pName = "main" },
		.layout = *layout }, NULL, &pipe));
	return pipe;
}

static VkDescriptorSet make_set(VkDescriptorSetLayout dsl)
{
	VkDescriptorPool dpool;
	VkDescriptorSet set;
	CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
		.poolSizeCount = 4, .pPoolSizes = (VkDescriptorPoolSize[]){
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 2 },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 2 } } }, NULL, &dpool));
	CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl }, &set));
	return set;
}

static void test_multiview(VkPipelineLayout pl0)
{
	const VkRenderPass saved_rp = rp;
	VkImage img;
	VkImageView views[2];
	VkFramebuffer f;
	VkPipeline pipe;
	VkCommandBuffer cmd;

	/* H1 */
	img = make_img(VK_FORMAT_R8G8B8A8_UNORM, W, H, 2, VK_SAMPLE_COUNT_1_BIT,
		       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	views[0] = make_view(img, VK_FORMAT_R8G8B8A8_UNORM, 2);
	const int mvx = getenv("VKGFX2_MVX") ? atoi(getenv("VKGFX2_MVX")) : 0;
	rp = make_rp(VK_SAMPLE_COUNT_1_BIT, 0, mvx == 1 || mvx >= 3 ? 0 : 3);
	f = make_fb(rp, views, 1);
	pipe = make_gfx((const char *[5]){ mvx == 5 ? "quadni.vert" : "full.vert", NULL, NULL, NULL,
					   mvx == 5 ? "col.frag" : mvx == 2 ? "red.frag" : "view.frag" },
			VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, pl0);
	cmd = begin_rp(rp, f);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	if (mvx == 5) {
		/* three draws that differ in their first instance */
		const char *seq = getenv("VKGFX2_SEQ");
		if (seq) {
			/* first instances from the environment, one digit each */
			for (; *seq; seq++) {
				if (*seq == 'z')
					vkCmdDraw(cmd, 0, 1, 0, 0);  /* nothing to draw */
				else
					vkCmdDraw(cmd, 6, 1, 0, *seq - '0');
			}
		} else {
			vkCmdDraw(cmd, 6, 1, 0, 3);
			vkCmdDraw(cmd, 3, 1, 0, 1);  /* half a quad */
			vkCmdDraw(cmd, 6, 1, 0, 2);
		}
	} else
		vkCmdDraw(cmd, 3, 1, 0, 0);
	if (mvx == 3)
		vkCmdDraw(cmd, 3, 1, 0, 1);  /* another draw with other parameters */
	if (mvx == 4)
		vkCmdDraw(cmd, 3, 1, 0, 0);  /* another draw with the same ones */
	vkCmdEndRenderPass(cmd);
	if (getenv("VKGFX2_SPLIT")) {
		/* the copy in a command buffer of its own */
		run(cmd);
		cmd = begin();
	}
	copy_out(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, W, H, 2);
	run(cmd);
	printf("H1 multiview: layer 0 red %d, layer 1 green %d (expect %d each; layer 0 green %d, "
	       "layer 1 red %d, layer 1 black %d)\n",
	       count(0, W * H, 255, 0, 0), count(W * H, W * H, 0, 255, 0), W * H,
	       count(0, W * H, 0, 255, 0), count(W * H, W * H, 255, 0, 0),
	       count(W * H, W * H, 0, 0, 0));
	if (mvx == 5) {
		int q[4] = { 0 }, x, y;
		for (y = 0; y < H; y++)
			for (x = 0; x < W; x++)
				q[(x >= 32) + 2 * (y >= 32)] += pix[(y * W + x) * 4 + 1] == 255 &&
								pix[(y * W + x) * 4] == 0;
		printf("three draws with first instance 3, 1 (half a quad), 2: quads drawn "
		       "%d %d %d %d (expect 0 about-128 256 256)\n", q[0], q[1], q[2], q[3]);
	}
	rp = saved_rp;
}

int main(void)
{
	VkInstance inst;
	uint32_t n = 8, next = 256, i;
	VkPhysicalDevice pd[8];
	VkPhysicalDeviceProperties props;
	static VkExtensionProperties exts[256];
	int have_xfb = 0, cnt, cnt2, bad;
	VkPhysicalDeviceTransformFeedbackFeaturesEXT xf = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
		.transformFeedback = VK_TRUE };
	VkPhysicalDeviceMultiviewFeatures mvf = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES,
		.multiview = VK_TRUE };
	const char *names[2] = { "VK_KHR_multiview", "VK_EXT_transform_feedback" };
	VkPipelineLayout pl0, plc;
	VkDescriptorSetLayout dsl;
	VkDescriptorSet set;
	VkImage img, img2;
	VkImageView views[2];
	VkFramebuffer f;
	VkPipeline pipe;
	VkCommandBuffer cmd;
	VkBuffer b0, b1, b2;
	uint32_t *w0, *w1, *w2;

	CHECK(vkCreateInstance(&(VkInstanceCreateInfo){
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }, NULL, &inst));
	CHECK(vkEnumeratePhysicalDevices(inst, &n, pd));
	pdev = pd[0];
	vkGetPhysicalDeviceProperties(pdev, &props);
	printf("device: %s\n", props.deviceName);
	vkEnumerateDeviceExtensionProperties(pdev, NULL, &next, exts);
	for (i = 0; i < next; i++)
		have_xfb |= !strcmp(exts[i].extensionName, "VK_EXT_transform_feedback");
	mvf.pNext = have_xfb ? &xf : NULL;
	CHECK(vkCreateDevice(pdev, &(VkDeviceCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &mvf,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
			.pQueuePriorities = (float[]){ 1.0f } },
		.enabledExtensionCount = have_xfb ? 2 : 1, .ppEnabledExtensionNames = names },
		NULL, &dev));
	vkGetDeviceQueue(dev, 0, 0, &queue);
	CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }, NULL, &pool));
	pix = make_buffer(&bpix, W * H * 4 * 2);
	CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO }, NULL, &pl0));

	/* H1 runs last by default; VKGFX2_MV_AT=<0..6> runs it before H2, H2b,
	 * H3, H4, H5, H6 or at the end (VKGFX2_MV_FIRST = 0), to look at how it
	 * combines with the others
	 */
	const int mv_at = getenv("VKGFX2_MV_AT") ? atoi(getenv("VKGFX2_MV_AT")) :
			  getenv("VKGFX2_MV_FIRST") ? 0 : 6;
	if (mv_at == 0)
		test_multiview(pl0);
	if (getenv("VKGFX2_IDS")) {
		/* what a compute shader sees of its ids and push constants here */
		uint32_t pcv[2] = { 0x1234, 0 };
		VkPipelineLayout pli;
		VkDescriptorSetLayout dsli;
		VkBuffer bi;
		uint32_t *wi = (uint32_t *)make_buffer(&bi, 4096);
		CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1,
			.pBindings = &(VkDescriptorSetLayoutBinding){
				.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT } },
			NULL, &dsli));
		CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1, .pSetLayouts = &dsli, .pushConstantRangeCount = 1,
			.pPushConstantRanges = &(VkPushConstantRange){
				VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 } }, NULL, &pli));
		CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = load_spv("ids.comp"),
				   .pName = "main" },
			.layout = pli }, NULL, &pipe));
		set = make_set(dsli);
		vkUpdateDescriptorSets(dev, 1, &(VkWriteDescriptorSet){
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
			.dstBinding = 0, .descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &(VkDescriptorBufferInfo){ bi, 0, VK_WHOLE_SIZE } }, 0, NULL);
		for (i = 0; i < 2; i++) {
		cmd = begin();
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pli, 0, 1, &set, 0, NULL);
		vkCmdPushConstants(cmd, pli, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pcv);
		vkCmdDispatch(cmd, 1, 1, 1);
		run(cmd);
		printf("ids: global x %08x %08x, group %08x %08x %08x, groups %08x %08x %08x, "
		       "push %08x (expect 0 1, 0 0 0, 1 1 1, 1234)\n", wi[0], wi[1], wi[64],
		       wi[256], wi[320], wi[128], wi[384], wi[448], wi[192]);
		}
		fflush(stdout);
	}

	/* H2 */
	img = make_img(VK_FORMAT_R8G8B8A8_UNORM, W, H, 1, VK_SAMPLE_COUNT_4_BIT,
		       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	img2 = make_img(VK_FORMAT_R8G8B8A8_UNORM, W, H, 1, VK_SAMPLE_COUNT_1_BIT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
			VK_IMAGE_USAGE_SAMPLED_BIT);
	views[0] = make_view(img, VK_FORMAT_R8G8B8A8_UNORM, 1);
	views[1] = make_view(img2, VK_FORMAT_R8G8B8A8_UNORM, 1);
	rp = make_rp(VK_SAMPLE_COUNT_4_BIT, 1, 0);
	f = make_fb(rp, views, 2);
	g_samples = VK_SAMPLE_COUNT_4_BIT;
	pipe = make_gfx((const char *[5]){ "half.vert", NULL, NULL, NULL, "red.frag" },
			VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, pl0);
	g_samples = VK_SAMPLE_COUNT_1_BIT;
	cmd = begin_rp(rp, f);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
	copy_out(cmd, img2, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, W, H, 1);
	run(cmd);
	for (cnt2 = 0, i = 0; i < W * H; i++)
		cnt2 += pix[i * 4] > 0 && pix[i * 4] < 255;
	printf("H2 4x multisample resolve: %d fully red, %d partly red on the edge, %d black "
	       "(expect about 1984, 64, 2048)\n", count(0, W * H, 255, 0, 0), cnt2,
	       count(0, W * H, 0, 0, 0));

	if (mv_at == 1)
		test_multiview(pl0);
	/* H2b: the four samples of each pixel on the triangle's long edge */
	{
		VkSampler samp;
		int hist[5] = { 0 }, k;
		w0 = (uint32_t *)make_buffer(&b0, 1 << 20);
		CHECK(vkCreateSampler(dev, &(VkSamplerCreateInfo){
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO }, NULL, &samp));
		int variant;
		VkImageView v1 = make_view(img2, VK_FORMAT_R8G8B8A8_UNORM, 1);
		for (variant = 0; variant < 2; variant++) {
		memset(w0, 0x55, 1 << 20);
		memset(hist, 0, sizeof(hist));
		pipe = make_comp(variant ? "msfetch.comp" : "fetch.comp", 2, (VkDescriptorSetLayoutBinding[]){
			{ .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			  .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
			{ .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			  .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT } },
			&plc, &dsl);
		{
			VkDescriptorPool dp;
			CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
				.poolSizeCount = 2, .pPoolSizes = (VkDescriptorPoolSize[]){
					{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
					{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } } }, NULL, &dp));
			CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = dp, .descriptorSetCount = 1,
				.pSetLayouts = &dsl }, &set));
		}
		vkUpdateDescriptorSets(dev, 2, (VkWriteDescriptorSet[]){
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
			  .dstBinding = 0, .descriptorCount = 1,
			  .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			  .pImageInfo = &(VkDescriptorImageInfo){
				samp, variant ? views[0] : v1,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } },
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
			  .dstBinding = 1, .descriptorCount = 1,
			  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			  .pBufferInfo = &(VkDescriptorBufferInfo){ b0, 0, VK_WHOLE_SIZE } } },
			0, NULL);
		cmd = begin();
		to_layout(cmd, variant ? img : img2, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, plc, 0, 1, &set, 0, NULL);
		vkCmdDispatch(cmd, 1, 1, 1);
		run(cmd);
		if (getenv("VKGFX_SCAN")) {
			/* where did the shader write? */
			for (i = 0, k = 0; i < (1 << 18) && k < 6; i++)
				if (w0[i] != 0x55555555) {
					printf("  word %u = %08x\n", i, w0[i]);
					k++;
				}
		}
		for (i = 0; i < 64; i++) {
			for (k = 0, cnt = 0; k < 4; k++)
				cnt += w0[i * 4 + k] == 255;
			hist[cnt]++;
		}
		printf("H2b %s of the 64 edge pixels: %d pixels with 0 red of 4, %d with 1, "
		       "%d with 2, %d with 3, %d with 4 (pixel 5: %u %u %u %u)\n",
		       variant ? "samples (texelFetch on the multisampled image)" :
				 "texelFetch on the resolved image", hist[0], hist[1],
		       hist[2], hist[3], hist[4], w0[20], w0[21], w0[22], w0[23]);
		fflush(stdout);
		}
	}

	if (mv_at == 2)
		test_multiview(pl0);
	/* H3: four yellow 16x16 quads, then blits to 32x32 */
	img = make_img(VK_FORMAT_R8G8B8A8_UNORM, W, H, 1, VK_SAMPLE_COUNT_1_BIT,
		       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	img2 = make_img(VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 1, VK_SAMPLE_COUNT_1_BIT,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	views[0] = make_view(img, VK_FORMAT_R8G8B8A8_UNORM, 1);
	rp = make_rp(VK_SAMPLE_COUNT_1_BIT, 0, 0);
	f = make_fb(rp, views, 1);
	pipe = make_gfx((const char *[5]){ "quadni.vert", NULL, NULL, NULL, "col.frag" },
			VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, pl0);
	for (i = 0; i < 2; i++) {
		cmd = begin_rp(rp, f);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdDraw(cmd, 6, 4, 0, 0);
		vkCmdEndRenderPass(cmd);
		to_layout(cmd, img2, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		vkCmdBlitImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img2,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &(VkImageBlit){
				.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
				.srcOffsets = { { 0, 0, 0 }, { W, H, 1 } },
				.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
				.dstOffsets = { { 0, 0, 0 }, { 32, 32, 1 } } },
			i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
		to_layout(cmd, img2, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		copy_out(cmd, img2, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 32, 32, 1);
		run(cmd);
		printf("H3 blit %s: %d cyan of 1024 (expect 256), %d black\n",
		       i ? "linear" : "nearest", count(0, 1024, 0, 255, 255),
		       count(0, 1024, 0, 0, 0));
	}
	cmd = begin();
	copy_out(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, W, H, 1);
	run(cmd);
	printf("H3 the blit's source: %d cyan of 4096 (expect 1024)\n",
	       count(0, W * H, 0, 255, 255));

	if (mv_at == 3)
		test_multiview(pl0);
	/* H4 */
	if (have_xfb) {
		void (*bind_xfb)(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer *,
				 const VkDeviceSize *, const VkDeviceSize *) =
			(void *)vkGetDeviceProcAddr(dev, "vkCmdBindTransformFeedbackBuffersEXT");
		void (*begin_xfb)(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer *,
				  const VkDeviceSize *) =
			(void *)vkGetDeviceProcAddr(dev, "vkCmdBeginTransformFeedbackEXT");
		void (*end_xfb)(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer *,
				const VkDeviceSize *) =
			(void *)vkGetDeviceProcAddr(dev, "vkCmdEndTransformFeedbackEXT");
		static const float want[12] = { -1, -1, 0, 7, 1, -1, 1, 7, -1, 1, 2, 7 };
		float *wf = (float *)make_buffer(&b0, 256);
		pipe = make_gfx((const char *[5]){ "xfb.vert", NULL, NULL, NULL, "red.frag" },
				VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, pl0);
		cmd = begin_rp(rp, f);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		bind_xfb(cmd, 0, 1, &b0, &(VkDeviceSize){ 0 }, &(VkDeviceSize){ 256 });
		begin_xfb(cmd, 0, 0, NULL, NULL);
		vkCmdDraw(cmd, 3, 1, 0, 0);
		end_xfb(cmd, 0, 0, NULL, NULL);
		vkCmdEndRenderPass(cmd);
		run(cmd);
		for (bad = 0, i = 0; i < 12; i++)
			bad += wf[i] != want[i];
		printf("H4 transform feedback: %d of 12 floats wrong (first vertex %g %g %g %g)\n",
		       bad, wf[0], wf[1], wf[2], wf[3]);
	} else {
		printf("H4 transform feedback: extension not offered\n");
	}

	if (mv_at == 4)
		test_multiview(pl0);
	/* H5 */
	w0 = (uint32_t *)make_buffer(&b0, 4096);
	w1 = (uint32_t *)make_buffer(&b1, 4096);
	img = make_img(VK_FORMAT_R32_UINT, 4, 4, 1, VK_SAMPLE_COUNT_1_BIT,
		       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
		       VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	views[0] = make_view(img, VK_FORMAT_R32_UINT, 1);
	pipe = make_comp("atom.comp", 2, (VkDescriptorSetLayoutBinding[]){
		{ .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		  .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
		{ .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		  .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT } }, &plc, &dsl);
	set = make_set(dsl);
	vkUpdateDescriptorSets(dev, 2, (VkWriteDescriptorSet[]){
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
		  .dstBinding = 0, .descriptorCount = 1,
		  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		  .pImageInfo = &(VkDescriptorImageInfo){
			VK_NULL_HANDLE, views[0], VK_IMAGE_LAYOUT_GENERAL } },
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
		  .dstBinding = 1, .descriptorCount = 1,
		  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		  .pBufferInfo = &(VkDescriptorBufferInfo){ b0, 0, VK_WHOLE_SIZE } } }, 0, NULL);
	cmd = begin();
	to_layout(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_GENERAL,
		&(VkClearColorValue){ .uint32 = { 0, 0, 0, 0 } }, 1,
		&(VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, plc, 0, 1, &set, 0, NULL);
	vkCmdDispatch(cmd, 8, 8, 1);
	vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_GENERAL, b1, 1,
		&(VkBufferImageCopy){ .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
				      .imageExtent = { 4, 4, 1 } });
	run(cmd);
	for (bad = 0, i = 0; i < 16; i++)
		bad += w1[i] != 256;
	printf("H5 image atomics: %d of 16 texels wrong (first %u, expect 256), flag %08x\n",
	       bad, w1[0], w0[0]);

	if (mv_at == 5)
		test_multiview(pl0);
	/* H6 */
	{
		VkBufferView vu, vs;
		w1 = (uint32_t *)make_buffer(&b1, 1024);
		w2 = (uint32_t *)make_buffer(&b2, 1024);
		for (i = 0; i < 256; i++) {
			w1[i] = i * 3;
			w2[i] = i + 1;
		}
		CHECK(vkCreateBufferView(dev, &(VkBufferViewCreateInfo){
			.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = b1,
			.format = VK_FORMAT_R32_UINT, .range = 1024 }, NULL, &vu));
		CHECK(vkCreateBufferView(dev, &(VkBufferViewCreateInfo){
			.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = b2,
			.format = VK_FORMAT_R32_UINT, .range = 1024 }, NULL, &vs));
		pipe = make_comp("tbuf.comp", 3, (VkDescriptorSetLayoutBinding[]){
			{ .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
			  .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
			{ .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			  .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
			{ .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
			  .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT } },
			&plc, &dsl);
		set = make_set(dsl);
		vkUpdateDescriptorSets(dev, 3, (VkWriteDescriptorSet[]){
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
			  .dstBinding = 0, .descriptorCount = 1,
			  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
			  .pTexelBufferView = &vs },
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
			  .dstBinding = 1, .descriptorCount = 1,
			  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			  .pBufferInfo = &(VkDescriptorBufferInfo){ b0, 0, VK_WHOLE_SIZE } },
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
			  .dstBinding = 2, .descriptorCount = 1,
			  .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
			  .pTexelBufferView = &vu } }, 0, NULL);
		cmd = begin();
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, plc, 0, 1, &set, 0, NULL);
		vkCmdDispatch(cmd, 4, 1, 1);
		run(cmd);
		for (bad = 0, cnt = 0, i = 0; i < 256; i++) {
			bad += w2[i] != i * 7 + 1;
			cnt += w0[i] != (256 | (256 << 16));
		}
		printf("H6 texel buffers: %d of 256 stored words wrong (word 5 = %u, expect 36), "
		       "%d sizes wrong (%08x)\n", bad, w2[5], cnt, w0[0]);
	}
	if (mv_at == 6)
		test_multiview(pl0);
	return 0;
}
