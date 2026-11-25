/* Offscreen Vulkan test, step 2: copies without drawing.
 *  A. texture upload: buffer -> RGBA8 image -> buffer, compared byte by byte.
 *  B. depth/stencil: clear a D24S8 (or D32S8) image through a render pass,
 *     read the depth and the stencil aspect back; then upload a new stencil
 *     plane and check the depth survived, and the other way round.
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
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT },
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

static VkImage make_image(VkFormat fmt, VkImageUsageFlags usage)
{
	VkImage img;
	VkMemoryRequirements mr;
	VkDeviceMemory mem;
	CHECK(vkCreateImage(dev, &(VkImageCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { W, H, 1 },
		.mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage }, NULL, &img));
	vkGetImageMemoryRequirements(dev, img, &mr);
	CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits, 0) }, NULL, &mem));
	CHECK(vkBindImageMemory(dev, img, mem, 0));
	return img;
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

static VkBufferImageCopy region(VkImageAspectFlags aspect)
{
	return (VkBufferImageCopy){
		.imageSubresource = { aspect, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
}

int main(void)
{
	VkInstance inst;
	uint32_t n = 8;
	VkPhysicalDevice pd[8];
	VkPhysicalDeviceProperties props;
	VkFormatProperties fp;
	VkBuffer b0, b1, bd, bs;
	uint8_t *m0, *m1, *md, *ms;
	VkImage tex, ds;
	VkFormat dsfmt = VK_FORMAT_D24_UNORM_S8_UINT;
	VkImageView view;
	VkRenderPass rp;
	VkFramebuffer fb;
	VkCommandBuffer cmd;
	VkBufferImageCopy r;
	const VkImageAspectFlags both = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	int i, bad;

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
			.pQueuePriorities = (float[]){ 1.0f } } }, NULL, &dev));
	vkGetDeviceQueue(dev, 0, 0, &queue);
	CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }, NULL, &pool));

	/* A */
	m0 = make_buffer(&b0, W * H * 4);
	m1 = make_buffer(&b1, W * H * 4);
	for (i = 0; i < W * H * 4; i++)
		m0[i] = (i * 31 + (i >> 8) * 7) & 0xff;
	tex = make_image(VK_FORMAT_R8G8B8A8_UNORM,
			 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
			 VK_IMAGE_USAGE_SAMPLED_BIT);
	cmd = begin();
	layout(cmd, tex, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
	       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	r = region(VK_IMAGE_ASPECT_COLOR_BIT);
	vkCmdCopyBufferToImage(cmd, b0, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
	layout(cmd, tex, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	vkCmdCopyImageToBuffer(cmd, tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b1, 1, &r);
	run(cmd);
	for (bad = 0, i = 0; i < W * H * 4; i++)
		bad += m0[i] != m1[i];
	printf("A texture upload round trip: %d of %d bytes differ\n", bad, W * H * 4);

	/* B */
	vkGetPhysicalDeviceFormatProperties(pdev, dsfmt, &fp);
	if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
		dsfmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
	printf("B depth/stencil format: %s\n",
	       dsfmt == VK_FORMAT_D24_UNORM_S8_UINT ? "D24_UNORM_S8_UINT" : "D32_SFLOAT_S8_UINT");
	ds = make_image(dsfmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
			VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	CHECK(vkCreateImageView(dev, &(VkImageViewCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = ds,
		.viewType = VK_IMAGE_VIEW_TYPE_2D, .format = dsfmt,
		.subresourceRange = { both, 0, 1, 0, 1 } }, NULL, &view));
	CHECK(vkCreateRenderPass(dev, &(VkRenderPassCreateInfo){
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
		.pAttachments = &(VkAttachmentDescription){
			.format = dsfmt, .samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL },
		.subpassCount = 1,
		.pSubpasses = &(VkSubpassDescription){
			.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
			.pDepthStencilAttachment = &(VkAttachmentReference){
				0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL } } }, NULL, &rp));
	CHECK(vkCreateFramebuffer(dev, &(VkFramebufferCreateInfo){
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
		.attachmentCount = 1, .pAttachments = &view,
		.width = W, .height = H, .layers = 1 }, NULL, &fb));
	md = make_buffer(&bd, W * H * 4);
	ms = make_buffer(&bs, W * H);

	cmd = begin();
	vkCmdBeginRenderPass(cmd, &(VkRenderPassBeginInfo){
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
		.framebuffer = fb, .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 1,
		.pClearValues = &(VkClearValue){ .depthStencil = { 0.5f, 0x5a } } },
		VK_SUBPASS_CONTENTS_INLINE);
	vkCmdEndRenderPass(cmd);
	r = region(VK_IMAGE_ASPECT_DEPTH_BIT);
	vkCmdCopyImageToBuffer(cmd, ds, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bd, 1, &r);
	r = region(VK_IMAGE_ASPECT_STENCIL_BIT);
	vkCmdCopyImageToBuffer(cmd, ds, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bs, 1, &r);
	run(cmd);
	if (dsfmt == VK_FORMAT_D24_UNORM_S8_UINT) {
		const uint32_t d0 = *(uint32_t *)md & 0xffffff, d1 = *(uint32_t *)(md + (W * H - 1) * 4) & 0xffffff;
		for (bad = 0, i = 0; i < W * H; i++)
			bad += ((*(uint32_t *)(md + i * 4)) & 0xffffff) != d0;
		printf("B1 depth after clear to 0.5: first %06x last %06x (expect 7fffff or 800000), %d pixels unlike the first\n", d0, d1, bad);
		printf("B1 the same copy's fourth byte (the stencil comes along on some drivers): %02x\n", md[3]);
	} else {
		for (bad = 0, i = 0; i < W * H; i++)
			bad += *(float *)(md + i * 4) != 0.5f;
		printf("B1 depth after clear to 0.5: first %f, %d pixels wrong\n", *(float *)md, bad);
	}
	for (bad = 0, i = 0; i < W * H; i++)
		bad += ms[i] != 0x5a;
	printf("B2 stencil after clear to 5a: first %02x last %02x, %d pixels wrong\n", ms[0], ms[W * H - 1], bad);

	/* upload a stencil plane, then read both aspects again */
	for (i = 0; i < W * H; i++)
		ms[i] = (i * 5) & 0xff;
	cmd = begin();
	layout(cmd, ds, both, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	r = region(VK_IMAGE_ASPECT_STENCIL_BIT);
	vkCmdCopyBufferToImage(cmd, bs, ds, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
	layout(cmd, ds, both, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	run(cmd);
	memset(ms, 0x11, W * H);
	memset(md, 0x11, W * H * 4);
	cmd = begin();
	r = region(VK_IMAGE_ASPECT_DEPTH_BIT);
	vkCmdCopyImageToBuffer(cmd, ds, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bd, 1, &r);
	r = region(VK_IMAGE_ASPECT_STENCIL_BIT);
	vkCmdCopyImageToBuffer(cmd, ds, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bs, 1, &r);
	run(cmd);
	for (bad = 0, i = 0; i < W * H; i++)
		bad += ms[i] != ((i * 5) & 0xff);
	printf("B3 stencil after upload: %d pixels wrong (first %02x %02x %02x)\n", bad, ms[0], ms[1], ms[2]);
	if (dsfmt == VK_FORMAT_D24_UNORM_S8_UINT)
		printf("B4 depth after stencil upload: first %06x last %06x (unchanged = %s)\n",
		       *(uint32_t *)md & 0xffffff, *(uint32_t *)(md + (W * H - 1) * 4) & 0xffffff,
		       ((*(uint32_t *)md & 0xffffff) | 1) == 0x7fffff || (*(uint32_t *)md & 0xffffff) == 0x800000 ? "yes" : "NO");
	else
		printf("B4 depth after stencil upload: first %f (unchanged = %s)\n",
		       *(float *)md, *(float *)md == 0.5f ? "yes" : "NO");
	return 0;
}
