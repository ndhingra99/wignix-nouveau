/* Offscreen Vulkan test, step 3: buffer commands without drawing.
 *  C1 vkCmdFillBuffer, C2 vkCmdUpdateBuffer, C3 vkCmdCopyBuffer (a large and
 *  an unaligned small range), C4 vkCmdSetEvent / vkCmdResetEvent.
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

#define N (300 * 1024)

int main(void)
{
	VkInstance inst;
	uint32_t n = 8;
	VkPhysicalDevice pd[8];
	VkPhysicalDeviceProperties props;
	VkBuffer b0, b1;
	uint8_t *m0, *m1;
	uint32_t *w0, upd[64];
	VkCommandBuffer cmd;
	VkEvent ev;
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

	m0 = make_buffer(&b0, N);
	m1 = make_buffer(&b1, N);
	w0 = (uint32_t *)m0;

	/* C1: fill words 16 .. N/4-16, the rest must stay 0x55 */
	cmd = begin();
	vkCmdFillBuffer(cmd, b0, 64, N - 128, 0xdeadbeef);
	run(cmd);
	for (bad = 0, i = 0; i < N / 4; i++)
		bad += w0[i] != (i >= 16 && i < N / 4 - 16 ? 0xdeadbeef : 0x55555555);
	printf("C1 fill: %d of %d words wrong (word 16 %08x, word 15 %08x)\n",
	       bad, N / 4, w0[16], w0[15]);

	/* C2: update 256 bytes at offset 1024 */
	for (i = 0; i < 64; i++)
		upd[i] = 0x01000000u * i + 0x00abcdef;
	cmd = begin();
	vkCmdUpdateBuffer(cmd, b0, 1024, sizeof(upd), upd);
	run(cmd);
	for (bad = 0, i = 0; i < 64; i++)
		bad += w0[256 + i] != upd[i];
	printf("C2 update: %d of 64 words wrong (before %08x, after %08x)\n",
	       bad, w0[255], w0[256 + 64]);

	/* C3: copy most of b0 to b1 and 13 bytes at an odd offset */
	for (i = 0; i < N; i++)
		m0[i] = (i * 13 + (i >> 9)) & 0xff;
	cmd = begin();
	vkCmdCopyBuffer(cmd, b0, b1, 2, (VkBufferCopy[]){
		{ 4096, 8192, N - 16384 }, { 3, 101, 13 } });
	run(cmd);
	for (bad = 0, i = 0; i < N - 16384; i++)
		bad += m1[8192 + i] != m0[4096 + i];
	printf("C3 copy: %d of %d bytes wrong", bad, N - 16384);
	for (bad = 0, i = 0; i < 13; i++)
		bad += m1[101 + i] != m0[3 + i];
	printf(", small range %d of 13 wrong, neighbours %02x %02x (expect 55 55)\n",
	       bad, m1[100], m1[114]);

	/* C4: events */
	CHECK(vkCreateEvent(dev, &(VkEventCreateInfo){
		.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO }, NULL, &ev));
	printf("C4 event: new %d", vkGetEventStatus(dev, ev));
	cmd = begin();
	vkCmdSetEvent(cmd, ev, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
	run(cmd);
	printf(", after set %d", vkGetEventStatus(dev, ev));
	cmd = begin();
	vkCmdCopyBuffer(cmd, b0, b1, 1, &(VkBufferCopy){ 0, 0, 4096 });
	vkCmdResetEvent(cmd, ev, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
	run(cmd);
	printf(", after reset %d (expect %d %d %d)\n", vkGetEventStatus(dev, ev),
	       VK_EVENT_RESET, VK_EVENT_SET, VK_EVENT_RESET);
	return 0;
}
