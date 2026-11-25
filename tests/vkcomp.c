/* Offscreen Vulkan test, step 4: a compute dispatch.
 *  D1 a compute shader writes one word per invocation into a storage
 *     buffer: index * mul + add (push constants), the work group id in bits
 *     16.. and the local invocation id in bits 24...
 *  D2 a 4x4x2 work group on a 3x2x2 grid: shared memory with a barrier, a
 *     uniform buffer, gl_NumWorkGroups and all three ids.
 *  D3 the same through vkCmdDispatchIndirect with other counts (2x3x1).
 *  D4 an indirect dispatch with a zero count: nothing may be written.
 *  usage: vkcomp [groups]   (fill.comp.spv, multi.comp.spv next to the program)
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
			VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT },
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

#define LOCAL 64

int main(int argc, char **argv)
{
	VkInstance inst;
	uint32_t n = 8, groups = argc > 1 ? atoi(argv[1]) : 16;
	VkPhysicalDevice pd[8];
	VkPhysicalDeviceProperties props;
	VkBuffer b0;
	uint32_t *w0, pc[2] = { 3, 7 };
	VkCommandBuffer cmd;
	VkShaderModule mod;
	VkDescriptorSetLayout dsl;
	VkPipelineLayout pl;
	VkPipeline pipe;
	VkDescriptorPool dpool;
	VkDescriptorSet set;
	static uint32_t code[16384];
	size_t code_size;
	FILE *f;
	uint32_t i, bad, first_bad = 0;

	if (!(f = fopen("fill.comp.spv", "rb")))
		return 1;
	code_size = fread(code, 1, sizeof(code), f);
	fclose(f);

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

	w0 = (uint32_t *)make_buffer(&b0, (groups + 1) * LOCAL * 4);

	CHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = code_size, .pCode = code }, NULL, &mod));
	CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1,
		.pBindings = &(VkDescriptorSetLayoutBinding){
			.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT } },
		NULL, &dsl));
	CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1,
		.pPushConstantRanges = &(VkPushConstantRange){
			VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 } }, NULL, &pl));
	CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main" },
		.layout = pl }, NULL, &pipe));
	printf("pipeline created\n");
	CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
		.poolSizeCount = 1, .pPoolSizes = &(VkDescriptorPoolSize){
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } }, NULL, &dpool));
	CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl }, &set));
	vkUpdateDescriptorSets(dev, 1, &(VkWriteDescriptorSet){
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
		.dstBinding = 0, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &(VkDescriptorBufferInfo){ b0, 0, VK_WHOLE_SIZE } }, 0, NULL);

	cmd = begin();
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
	vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
	vkCmdDispatch(cmd, groups, 1, 1);
	run(cmd);

	for (bad = 0, i = 0; i < groups * LOCAL; i++) {
		uint32_t want = i * pc[0] + pc[1] + (i / LOCAL) * 0x10000u + (i % LOCAL) * 0x1000000u;
		if (w0[i] != want && !bad++)
			first_bad = i;
	}
	printf("D1 dispatch %u x %u: %u of %u words wrong", groups, LOCAL, bad, groups * LOCAL);
	if (bad)
		printf(" (first at %u: %08x, expect %08x)", first_bad, w0[first_bad],
		       first_bad * pc[0] + pc[1] + (first_bad / LOCAL) * 0x10000u +
		       (first_bad % LOCAL) * 0x1000000u);
	printf(", word past the end %08x (expect 55555555)\n", w0[groups * LOCAL]);

	/* D2..D4 */
	{
		VkBuffer bo, bu, bi;
		uint32_t *wo, *wu, *wi, gx, gy, gz, t;
		VkDescriptorSetLayout dsl2;
		VkPipelineLayout pl2;
		VkPipeline pipe2;
		VkDescriptorPool dpool2;
		VkDescriptorSet set2;
		static const uint32_t cases[3][3] = { { 3, 2, 2 }, { 2, 3, 1 }, { 2, 0, 1 } };

		if (!(f = fopen("multi.comp.spv", "rb")))
			return 1;
		code_size = fread(code, 1, sizeof(code), f);
		fclose(f);
		wo = (uint32_t *)make_buffer(&bo, 4096 * 4);
		wu = (uint32_t *)make_buffer(&bu, 256);
		wi = (uint32_t *)make_buffer(&bi, 256);
		for (i = 0; i < 16; i++)
			wu[i] = 0;
		wu[4] = 1000; /* k[1] in std140: 16 bytes per element */
		CHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){
			.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = code_size, .pCode = code }, NULL, &mod));
		CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2,
			.pBindings = (VkDescriptorSetLayoutBinding[]){
				{ .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				  .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
				{ .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				  .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT } } },
			NULL, &dsl2));
		CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1, .pSetLayouts = &dsl2 }, NULL, &pl2));
		CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main" },
			.layout = pl2 }, NULL, &pipe2));
		CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
			.poolSizeCount = 2, .pPoolSizes = (VkDescriptorPoolSize[]){
				{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
				{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 } } }, NULL, &dpool2));
		CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = dpool2, .descriptorSetCount = 1, .pSetLayouts = &dsl2 }, &set2));
		vkUpdateDescriptorSets(dev, 2, (VkWriteDescriptorSet[]){
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set2,
			  .dstBinding = 0, .descriptorCount = 1,
			  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			  .pBufferInfo = &(VkDescriptorBufferInfo){ bo, 0, VK_WHOLE_SIZE } },
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set2,
			  .dstBinding = 1, .descriptorCount = 1,
			  .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			  .pBufferInfo = &(VkDescriptorBufferInfo){ bu, 0, 64 } } }, 0, NULL);

		for (t = 0; t < 3; t++) {
			uint32_t total, x, y, z, nx, ny;
			gx = cases[t][0]; gy = cases[t][1]; gz = cases[t][2];
			memset(wo, 0x55, 4096 * 4);
			wi[4] = gx; wi[5] = gy; wi[6] = gz;
			cmd = begin();
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe2);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl2, 0, 1, &set2, 0, NULL);
			if (t == 0)
				vkCmdDispatch(cmd, gx, gy, gz);
			else
				vkCmdDispatchIndirect(cmd, bi, 16);
			run(cmd);
			nx = gx * 4; ny = gy * 4;
			total = nx * ny * gz * 2;
			for (bad = 0, z = 0; z < gz * 2; z++)
				for (y = 0; y < ny; y++)
					for (x = 0; x < nx; x++) {
						uint32_t li = (x & 3) + 4 * (y & 3) + 16 * (z & 1);
						uint32_t want = (31 - li) * 3 + 1000 + (x & 7) * (z / 2) +
							gx * 0x100 + gy * 0x10000 + gz * 0x1000000;
						uint32_t idx = x + nx * (y + ny * z);
						if (wo[idx] != want && !bad++)
							first_bad = idx;
					}
			printf("D%u %s %ux%ux%u: %u of %u words wrong", t + 2,
			       t ? "indirect" : "direct", gx, gy, gz, bad, total);
			if (bad)
				printf(" (first at %u: %08x)", first_bad, wo[first_bad]);
			printf(", word past the end %08x (expect 55555555)\n", wo[total]);
		}
	}
	return 0;
}
