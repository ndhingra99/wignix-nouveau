/* Offscreen Vulkan test, step 5: images in compute shaders (64x48 RGBA8).
 *  I1 a compute shader samples a texture (nearest, texel centres) and writes
 *     the texels to a storage buffer.
 *  I2 a compute shader writes a pattern with imageStore; the image is then
 *     copied to a buffer.
 *  I3 a compute shader reads that image with imageLoad into a storage buffer.
 *  I4 an image with linear tiling, written through a mapping, copied to an
 *     ordinary image with vkCmdCopyImage and read back (what vkcube does
 *     with its texture).
 *  I5 copies of images whose size is no multiple of 8 and whose format is
 *     BGRA (UNORM and sRGB): optimal to linear with vkCmdCopyImage, and
 *     optimal to buffer (what a presentation path does).
 *  tex.comp.spv, store.comp.spv, load.comp.spv next to the program.
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

static void dispatch(VkPipeline pipe, VkDescriptorType type0, VkImageView view,
		     VkSampler samp, VkImageLayout il, VkBuffer out)
{
	VkDescriptorPool dpool;
	VkDescriptorSet set;
	VkCommandBuffer cmd;
	CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
		.poolSizeCount = 2, .pPoolSizes = (VkDescriptorPoolSize[]){
			{ type0, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } } }, NULL, &dpool));
	CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl }, &set));
	vkUpdateDescriptorSets(dev, 2, (VkWriteDescriptorSet[]){
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
		  .dstBinding = 0, .descriptorCount = 1, .descriptorType = type0,
		  .pImageInfo = &(VkDescriptorImageInfo){ samp, view, il } },
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
		  .dstBinding = 1, .descriptorCount = 1,
		  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		  .pBufferInfo = &(VkDescriptorBufferInfo){ out, 0, VK_WHOLE_SIZE } } }, 0, NULL);
	cmd = begin();
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
	vkCmdDispatch(cmd, W / 8, H / 8, 1);
	run(cmd);
}

static VkImageView make_view(VkImage img)
{
	VkImageView view;
	CHECK(vkCreateImageView(dev, &(VkImageViewCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
		.viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } }, NULL, &view));
	return view;
}

int main(void)
{
	VkInstance inst;
	uint32_t n = 8;
	VkPhysicalDevice pd[8];
	VkPhysicalDeviceProperties props;
	VkBuffer b0, b1;
	uint8_t *m0, *m1;
	VkImage tex, simg;
	VkImageView tview, sview;
	VkSampler samp;
	VkPipeline pipe;
	VkCommandBuffer cmd;
	VkBufferImageCopy r;
	int i, bad, first;

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

	m0 = make_buffer(&b0, W * H * 4);
	m1 = make_buffer(&b1, W * H * 4);

	if (getenv("VKIMG_ONLY5"))
		goto i5;

	/* I1 */
	for (i = 0; i < W * H * 4; i++)
		m0[i] = (i * 31 + (i >> 8) * 7) & 0xff;
	tex = make_image(VK_FORMAT_R8G8B8A8_UNORM,
			 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	tview = make_view(tex);
	CHECK(vkCreateSampler(dev, &(VkSamplerCreateInfo){
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE }, NULL, &samp));
	cmd = begin();
	layout(cmd, tex, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
	       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	r = region(VK_IMAGE_ASPECT_COLOR_BIT);
	vkCmdCopyBufferToImage(cmd, b0, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
	layout(cmd, tex, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	run(cmd);
	pipe = make_pipe("tex.comp.spv", VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	printf("I1 pipeline created\n");
	dispatch(pipe, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, tview, samp,
		 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, b1);
	for (bad = 0, first = -1, i = 0; i < W * H * 4; i++)
		if (m1[i] != m0[i] && !bad++)
			first = i;
	printf("I1 texture sampled in compute: %d of %d bytes wrong", bad, W * H * 4);
	if (bad)
		printf(" (first at %d: %02x, expect %02x)", first, m1[first], m0[first]);
	printf("\n");

	/* I2 */
	simg = make_image(VK_FORMAT_R8G8B8A8_UNORM,
			  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	sview = make_view(simg);
	cmd = begin();
	layout(cmd, simg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
	       VK_IMAGE_LAYOUT_GENERAL);
	run(cmd);
	pipe = make_pipe("store.comp.spv", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	printf("I2 pipeline created\n");
	dispatch(pipe, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sview, VK_NULL_HANDLE,
		 VK_IMAGE_LAYOUT_GENERAL, b1);
	memset(m0, 0x11, W * H * 4);
	cmd = begin();
	layout(cmd, simg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
	       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	r = region(VK_IMAGE_ASPECT_COLOR_BIT);
	vkCmdCopyImageToBuffer(cmd, simg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b0, 1, &r);
	layout(cmd, simg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	       VK_IMAGE_LAYOUT_GENERAL);
	run(cmd);
	for (bad = 0, first = -1, i = 0; i < W * H; i++) {
		int x = i % W, y = i / W;
		uint8_t want[4] = { x * 4, y * 5, x ^ y, 255 };
		if (memcmp(m0 + i * 4, want, 4) && !bad++)
			first = i;
	}
	if (bad && getenv("VKIMG_MAP")) {
		/* which texel's value arrived at each place in the first rows */
		int x, y;
		for (y = 0; y < 16; y++) {
			printf("map y=%2d:", y);
			for (x = 0; x < 20; x += 4)
				printf(" x%-2d<-(%2d,%2d)", x, m0[(y * W + x) * 4] / 4,
				       m0[(y * W + x) * 4 + 1] / 5);
			printf("\n");
		}
	}
	printf("I2 imageStore: %d of %d pixels wrong", bad, W * H);
	if (bad)
		printf(" (first at %d: %02x %02x %02x %02x)", first, m0[first * 4],
		       m0[first * 4 + 1], m0[first * 4 + 2], m0[first * 4 + 3]);
	printf("\n");

	/* I3 */
	memset(m1, 0x22, W * H * 4);
	pipe = make_pipe("load.comp.spv", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	printf("I3 pipeline created\n");
	dispatch(pipe, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sview, VK_NULL_HANDLE,
		 VK_IMAGE_LAYOUT_GENERAL, b1);
	for (bad = 0, first = -1, i = 0; i < W * H * 4; i++)
		if (m1[i] != m0[i] && !bad++)
			first = i;
	printf("I3 imageLoad: %d of %d bytes differ from the image copy", bad, W * H * 4);
	if (bad)
		printf(" (first at %d: %02x, expect %02x)", first, m1[first], m0[first]);
	printf("\n");

	/* I4 */
	{
		VkImage lin, opt;
		VkMemoryRequirements mr;
		VkDeviceMemory mem;
		VkSubresourceLayout sl;
		uint8_t *lm;
		int x, y;
		CHECK(vkCreateImage(dev, &(VkImageCreateInfo){
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
			.extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
			.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
			.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED }, NULL, &lin));
		vkGetImageMemoryRequirements(dev, lin, &mr);
		CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
			.memoryTypeIndex = mem_type(mr.memoryTypeBits,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
				VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) }, NULL, &mem));
		CHECK(vkBindImageMemory(dev, lin, mem, 0));
		CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&lm));
		vkGetImageSubresourceLayout(dev, lin,
			&(VkImageSubresource){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }, &sl);
		for (y = 0; y < H; y++)
			for (x = 0; x < W; x++) {
				uint8_t *t = lm + sl.offset + y * sl.rowPitch + x * 4;
				t[0] = x * 3; t[1] = y * 5; t[2] = x + y; t[3] = 255;
			}
		opt = make_image(VK_FORMAT_R8G8B8A8_UNORM,
				 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
				 VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
		memset(m1, 0x33, W * H * 4);
		cmd = begin();
		layout(cmd, lin, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_PREINITIALIZED,
		       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		layout(cmd, opt, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
		       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		vkCmdCopyImage(cmd, lin, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, opt,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &(VkImageCopy){
				.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
				.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
				.extent = { W, H, 1 } });
		layout(cmd, opt, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		r = region(VK_IMAGE_ASPECT_COLOR_BIT);
		vkCmdCopyImageToBuffer(cmd, opt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b1, 1, &r);
		run(cmd);
		for (bad = 0, i = 0; i < W * H; i++) {
			const uint8_t want[4] = { (i % W) * 3, (i / W) * 5, i % W + i / W, 255 };
			bad += memcmp(want, m1 + i * 4, 4) != 0;
		}
		printf("I4 linear image copied to an ordinary one: %d of %d pixels wrong "
		       "(row pitch %u)\n", bad, W * H, (unsigned)sl.rowPitch);
	}

	/* I5 */
i5:
	{
		static const VkFormat fmts[3] = { VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM,
						  VK_FORMAT_B8G8R8A8_UNORM };
		const uint32_t w = 100, h = 60;
		VkBuffer bb, bc;
		uint8_t *mb = make_buffer(&bb, w * h * 4), *mc = make_buffer(&bc, w * h * 4);
		int f;
		for (f = 0; f < 3; f++) {
			VkImage a, lin;
			VkMemoryRequirements mr;
			VkDeviceMemory mem;
			VkSubresourceLayout sl;
			uint8_t *lm;
			uint32_t x, y;
			int bad2 = 0;
			const VkImageCreateInfo ci = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
				.imageType = VK_IMAGE_TYPE_2D, .format = fmts[f],
				.extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
				.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
				.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
					 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT };
			VkImageCreateInfo li = ci;
			CHECK(vkCreateImage(dev, &ci, NULL, &a));
			vkGetImageMemoryRequirements(dev, a, &mr);
			CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
				.memoryTypeIndex = mem_type(mr.memoryTypeBits, 0) }, NULL, &mem));
			CHECK(vkBindImageMemory(dev, a, mem, 0));
			li.tiling = VK_IMAGE_TILING_LINEAR;
			li.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			CHECK(vkCreateImage(dev, &li, NULL, &lin));
			vkGetImageMemoryRequirements(dev, lin, &mr);
			CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
				.memoryTypeIndex = mem_type(mr.memoryTypeBits,
					VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
					VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) }, NULL, &mem));
			CHECK(vkBindImageMemory(dev, lin, mem, 0));
			CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&lm));
			vkGetImageSubresourceLayout(dev, lin,
				&(VkImageSubresource){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }, &sl);
			for (i = 0; i < (int)(w * h * 4); i++)
				mb[i] = (i * 7 + (i >> 9)) & 0xff;
			memset(mc, 0x44, w * h * 4);
			memset(lm, 0x44, mr.size);
			cmd = begin();
			layout(cmd, a, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
			       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			vkCmdCopyBufferToImage(cmd, bb, a, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
				&(VkBufferImageCopy){
					.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
					.imageExtent = { w, h, 1 } });
			layout(cmd, a, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			layout(cmd, lin, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
			       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			if (getenv("VKIMG_SPLIT")) {
				run(cmd);
				cmd = begin();
			}
			if (getenv("VKIMG_NOIMG"))
				goto skip_img;
			vkCmdCopyImage(cmd, a, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, lin,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &(VkImageCopy){
					.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
					.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
					.extent = { w, h, 1 } });
			if (getenv("VKIMG_SPLIT")) {
				run(cmd);
				cmd = begin();
			}
skip_img:
			vkCmdCopyImageToBuffer(cmd, a, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bc, 1,
				&(VkBufferImageCopy){
					.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
					.imageExtent = { w, h, 1 } });
			run(cmd);
			for (bad = 0, i = 0; i < (int)(w * h * 4); i++)
				bad += mc[i] != mb[i];
			for (y = 0; y < h; y++)
				for (x = 0; x < w * 4; x++)
					bad2 += lm[sl.offset + y * sl.rowPitch + x] != mb[y * w * 4 + x];
			printf("I5 %s 100x60: to buffer %d bytes wrong, to linear image %d bytes wrong "
			       "(of %u)\n", f ? "BGRA8" : "BGRA8 sRGB", bad, bad2, w * h * 4);
			if (bad)
				printf("   first bytes %02x %02x %02x %02x %02x %02x %02x %02x, expect "
				       "%02x %02x %02x %02x %02x %02x %02x %02x; row 1: %02x %02x, expect "
				       "%02x %02x\n", mc[0], mc[1], mc[2], mc[3], mc[4], mc[5], mc[6], mc[7],
				       mb[0], mb[1], mb[2], mb[3], mb[4], mb[5], mb[6], mb[7],
				       mc[400], mc[401], mb[400], mb[401]);
			fflush(stdout);
		}
	}
	return 0;
}
