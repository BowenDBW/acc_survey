/*
 * kgsl_probe.c — 探测无 root 下能否通过 /dev/kgsl-3d0 的 GETPROPERTY ioctl
 * 读到 Adreno GPU 的时钟 / 占用 / 温度。
 *
 * 背景：/sys/class/kgsl/kgsl-3d0/ 下各文件在 MIUI 上对 shell 是 DAC 拒绝
 * （Permission denied），但设备节点 /dev/kgsl-3d0 是 0666 人人可开。kgsl 有
 * IOCTL_KGSL_DEVICE_GETPROPERTY(0x2) 能直接向内核要 GPU 属性。
 *
 * 由于不同内核版本属性号不同，这里暴力尝试 type 0x1..0x40，
 * 看哪些返回 0，再把值打印出来人工判读（时钟 ≈ 几百 MHz、占用 0-100、温度 ≈ 30-40）。
 *
 * 编译（NDK r26c, aarch64, API 24）:
 *   $NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android24-clang \
 *       kgsl_probe.c -o kgsl_probe
 * 运行:
 *   adb push kgsl_probe /data/local/tmp/
 *   adb shell chmod 755 /data/local/tmp/kgsl_probe
 *   adb shell /data/local/tmp/kgsl_probe
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define KGSL_IOC_TYPE 0x2A
/* 64 位下 struct = uint(4) + pad(4) + ptr(8) + size_t(8) = 24 字节，
   ioctl 号里编码的 size 必须是 24，否则内核直接 EINVAL。 */
#define IOCTL_KGSL_DEVICE_GETPROPERTY \
	_IOWR(KGSL_IOC_TYPE, 0x2, struct kgsl_device_getproperty)

struct kgsl_device_getproperty {
	unsigned int type;
	void *value;
	size_t sizebytes;
};

int main(void) {
	int fd = open("/dev/kgsl-3d0", O_RDWR);
	if (fd < 0) {
		printf("open /dev/kgsl-3d0 FAILED: %s (errno=%d)\n", strerror(errno), errno);
		return 1;
	}
	printf("opened /dev/kgsl-3d0 fd=%d (uid=%d)\n\n", fd, getuid());

	printf("== pass A: 4 字节缓冲 (type 0x1..0x100) ==\n");
	for (unsigned int t = 0x1; t <= 0x100; t++) {
		uint32_t val = 0;
		struct kgsl_device_getproperty p = { .type = t, .value = &val, .sizebytes = sizeof(val) };
		errno = 0;
		int r = ioctl(fd, IOCTL_KGSL_DEVICE_GETPROPERTY, &p);
		if (r == 0)
			printf("  type=0x%02X  OK  val=%u (0x%08X)\n", t, val, val);
		else if (errno != EINVAL && errno != EPERM)
			printf("  type=0x%02X  err=%s\n", t, strerror(errno));
	}

	printf("\n== pass B: 64 字节缓冲 ==\n");
	for (unsigned int t = 0x1; t <= 0x100; t++) {
		unsigned char buf[64];
		memset(buf, 0, sizeof(buf));
		struct kgsl_device_getproperty p = { .type = t, .value = buf, .sizebytes = sizeof(buf) };
		errno = 0;
		int r = ioctl(fd, IOCTL_KGSL_DEVICE_GETPROPERTY, &p);
		if (r == 0) {
			unsigned long v = 0;
			memcpy(&v, buf, sizeof(v));
			printf("  type=0x%02X  OK  first8=%lu (0x%lX)\n", t, v, v);
		}
	}

	close(fd);
	return 0;
}
