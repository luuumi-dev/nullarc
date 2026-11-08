/* SPDX-License-Identifier: MIT
 * Read-only AMDGPU_INFO capture. Build on Linux with installed DRM headers:
 * cc -std=c11 -Wall -Wextra -Werror $(pkg-config --cflags libdrm) collect.c -o collect
 * ./collect /dev/dri/renderD128 > hp-760m.json 2> hp-ip-blocks.txt
 * No register access, firmware loading, or device reconfiguration.
 */
#define _POSIX_C_SOURCE 200809L
#include <amdgpu_drm.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static uint32_t pci_value(const struct stat *st, const char *name) {
    char path[256]; unsigned value;
    snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/device/%s",
             major(st->st_rdev), minor(st->st_rdev), name);
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    int ok = fscanf(f, "%x", &value); fclose(f);
    if (ok != 1) { fprintf(stderr, "invalid PCI sysfs field: %s\n", name); exit(1); }
    return value;
}
int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: collect /dev/dri/renderD128\n"); return 1; }
    int fd = open(argv[1], O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISCHR(st.st_mode)) { perror("DRM node"); return 1; }
    uint32_t vendor = pci_value(&st, "vendor"), device = pci_value(&st, "device");
    uint32_t revision = pci_value(&st, "revision");
    uint32_t subvendor = pci_value(&st, "subsystem_vendor"), subdevice = pci_value(&st, "subsystem_device");
    if (vendor != 0x1002) { fprintf(stderr, "not AMD\n"); return 1; }
    struct drm_amdgpu_info_device d = {0};
    struct drm_amdgpu_info q = {0};
    q.query = AMDGPU_INFO_DEV_INFO; q.return_pointer = (uintptr_t)&d; q.return_size = sizeof(d);
    if (ioctl(fd, DRM_IOCTL_AMDGPU_INFO, &q)) { perror("AMDGPU_INFO_DEV_INFO"); return 1; }
    if (d.device_id != device || d.pci_rev != revision) { fprintf(stderr, "DRM and sysfs identity disagree\n"); return 1; }
    printf("{\n  \"format\": 1,\n  \"pci\": {\"vendor\": %u, \"device\": %u, \"revision\": %u, \"subvendor\": %u, \"subdevice\": %u},\n",
           vendor, device, revision, subvendor, subdevice);
    printf("  \"num_shader_engines\": %u,\n  \"num_shader_arrays_per_engine\": %u,\n  \"num_cu_per_sh\": %u,\n",
           d.num_shader_engines, d.num_shader_arrays_per_engine, d.num_cu_per_sh);
    printf("  \"num_rb_pipes\": %u,\n  \"enabled_rb_pipes_mask\": %u,\n  \"cu_active_number\": %u,\n  \"cu_bitmap\": [\n",
           d.num_rb_pipes, d.enabled_rb_pipes_mask, d.cu_active_number);
    for (unsigned i = 0; i < 4; i++)
        printf("    [%u, %u, %u, %u]%s\n", d.cu_bitmap[i][0], d.cu_bitmap[i][1], d.cu_bitmap[i][2], d.cu_bitmap[i][3], i < 3 ? "," : "");
    puts("  ]\n}");
    for (unsigned type = 0; type < AMDGPU_HW_IP_NUM; ++type) {
        struct drm_amdgpu_info_hw_ip ip = {0};
        q.query = AMDGPU_INFO_HW_IP_INFO; q.return_pointer = (uintptr_t)&ip; q.return_size = sizeof(ip);
        q.query_hw_ip.type = type; q.query_hw_ip.ip_instance = 0;
        if (!ioctl(fd, DRM_IOCTL_AMDGPU_INFO, &q))
            fprintf(stderr, "type=%u version=%u.%u discovery=0x%08x available_rings=0x%x\n",
                    type, ip.hw_ip_version_major, ip.hw_ip_version_minor, ip.ip_discovery_version, ip.available_rings);
    }
    close(fd);
    return ferror(stdout) ? 1 : 0;
}
