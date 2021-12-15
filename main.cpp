#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/sysmacros.h>

#include <xf86drmMode.h>
#include <libdrm/drm.h>
#include <libdrm/radeon_drm.h>

#include <unordered_map>
#include <string>
#include <vector>

/* This files is pulled from the linux kernel */
#include "r100d.h"
#include "r200_reg.h"

using namespace std;

#define STR_SIZE(x) sizeof((x)), strdup(x)

#define PCI_CHIP_RV250_If 0x4966
#define MAX_VERTEX_ARRAYS 16

extern "C"
{
    void my_lib_load() __attribute__((constructor));
    int open64(const char *path, int oflag, ...);
    int fstat64(int fd, struct stat *buf);
    int ioctl(int fd, unsigned long request, ...);
    void* mmap64(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
}

typedef int (*my_fstat_t)(int fd, struct stat *buf);
typedef int (*my_open_t)(const char *path, int oflag);
typedef int (*my_ioctl_t)(int fd, unsigned long request, ...);
typedef void* (*my_mmap64_t)(void *addr, size_t length, int prot, int flags, int fd, off_t offset);

static my_fstat_t my_fstat;
static my_open_t my_open;
static my_ioctl_t my_ioctl;
static my_mmap64_t my_mmap64;

static int dri_file = -1;

static drm_mode_card_res res;

static drm_mode_modeinfo mode_info;

static drm_mode_get_connector con_info;

static drm_mode_get_encoder enc = 
{
    1,
    DRM_MODE_ENCODER_VIRTUAL,
    1,
    1,
    1,
};

static struct drm_radeon_gem_info radeon_gem_info =
{
    32*1024*1024,
    32*1024*1024,
    true,
};

static struct drm_version version;

static drm_radeon_gem_get_tiling get_tile_info =
{
    0,
    0,
    0,
};

static unordered_map<uint32_t, uint32_t> radeon_info =
{
    {RADEON_INFO_DEVICE_ID, PCI_CHIP_RV250_If},
};

static vector<uint8_t*> gem_buffers;

//static uint32_t registers[64*1024];

void my_get_resources(struct drm_mode_card_res *o_res)
{
    if(o_res->fb_id_ptr == 0)
    {
        memcpy(o_res, &res, sizeof(res));
    }
    else
    {
        ((uint32_t*)o_res->fb_id_ptr)[0] = 1;
        ((uint32_t*)o_res->crtc_id_ptr)[0] = 1;
        ((uint32_t*)o_res->connector_id_ptr)[0] = 1;
        ((uint32_t*)o_res->encoder_id_ptr)[0] = 1;
    }
}

void my_get_connector(struct drm_mode_get_connector *o_conn)
{
    if(o_conn->encoders_ptr == 0)
    {
        memcpy(o_conn, &con_info, sizeof(struct drm_mode_get_connector));
    }
    else
    {
        ((uint32_t*)o_conn->encoders_ptr)[0] = 1;
        ((uint32_t*)o_conn->props_ptr)[0] = 1;
        ((uint64_t*)o_conn->prop_values_ptr)[0] = 0;
        ((drm_mode_modeinfo*)o_conn->modes_ptr)[0] = mode_info;
    }
}

void my_send_version(struct drm_version *ver)
{
    if(ver->name_len == 0)
    {
        memcpy(ver, &version, sizeof(version));
    }
    else
    {
        strcpy(ver->name, version.name);
        strcpy(ver->date, version.date);
        strcpy(ver->desc, version.desc);
    }
}

int my_send_radeon_info(struct drm_radeon_info *r_info)
{
    if(radeon_info.find(r_info->request) == radeon_info.end())
    {
        printf("radeon_info unknown request: 0x%x\n", r_info->request);
        return -1;
    }

    ((uint32_t*)r_info->value)[0] = radeon_info[r_info->request];
    return 0;
}

void my_gem_create(struct drm_radeon_gem_create *gem)
{
    uint8_t *data = new uint8_t[gem->size];
    int handle = gem_buffers.size();
    gem_buffers.push_back(data);

    printf("Allocated buffer %d of size %llu\n", handle, gem->size);

    gem->handle = handle;
    return;
}

void my_gem_flink(struct drm_gem_flink *flink)
{
    flink->name = 0;
}

void my_gem_mmap(struct drm_radeon_gem_mmap *gem_mmap)
{
    printf("Handle %u\n", gem_mmap->handle);
    printf("Offset %llu\n", gem_mmap->offset);
    printf("Size %llu\n", gem_mmap->size);
    printf("Addr ptr %llu\n", gem_mmap->addr_ptr);

    gem_mmap->addr_ptr = (uintptr_t)(gem_buffers[gem_mmap->handle] + gem_mmap->offset);
    return;
}

void my_parse_packets(uint32_t *packets, size_t num_packets)
{
    int packet_type = -1;
    int64_t count;
    size_t cur_reg;
    uint32_t op_code;
    uint8_t *v_addrs[MAX_VERTEX_ARRAYS];
    for(size_t i = 0; i < num_packets; i++)
    {
        uint32_t &val = packets[i];
        if(packet_type == -1)
        {
            switch(val & CP_PACKET3)
            {
                case CP_PACKET0:
                    cur_reg = (val & PACKET0_BASE_INDEX_MASK) << 2;
                    count = ((val & PACKET0_COUNT_MASK) >> PACKET0_COUNT_SHIFT)+1;
                    packet_type = 0;
                    printf("CP0 on 0x%lx with count %lu\n", cur_reg, count);
                    break;
                case CP_PACKET1:
                    printf("Got CP_PACKET1 don't know how to handle this\n");
                    break;
                case CP_PACKET2:
                    /* This is a NOP */
                    break;
                case CP_PACKET3:
                    count = ((val & PACKET3_COUNT_MASK) >> PACKET3_COUNT_SHIFT);
                    op_code = (val & PACKET3_IT_OPCODE_MASK) >> PACKET3_IT_OPCODE_SHIFT;
                    packet_type = 3;
                    printf("Count is %ld\n", count);
                    break;
            }
        }
        else if(packet_type == 0)
        {
            printf("Updating register 0x%08lx with value 0x%08x\n", cur_reg, val);
            cur_reg += 4;
            count -= 1;
            if(count == 0)
            {
                packet_type = -1;
            }
        }
        else if(packet_type == 3)
        {
            switch(op_code)
            {
                case PACKET3_3D_LOAD_VBPNTR:
                    {
                        uint32_t num_arrays = packets[i++];
                        printf("Num arrays %u\n", num_arrays);
                        for(uint32_t j = 0; j < num_arrays; j++)
                        {
                            if((j%2) == 0)
                            {
                                printf("Vertex attributes 0x%x\n", packets[i++]);
                                count -= 1;
                            }
                            v_addrs[j] = (uint8_t*)(uintptr_t)packets[i++];
                            printf("Vertex pointer 0x%lx\n", (size_t)v_addrs[j]);
                            count -= 1;
                        }

                        for(uint32_t j = 0; j < num_arrays; j++)
                        {
                            /* Reloc should have magic value before it */
                            if(packets[i++] == 0xc0001000)
                            {
                                size_t handle = packets[i++];
                                v_addrs[j] = &gem_buffers[handle][0] + (uintptr_t)v_addrs[j]; 
                            }
                        }

                        /* Decrement i as we would have incremented one more time than we need */
                        i--;
                    } 
                    break;
                case PACKET3_NOP:
                    count -= 1;
                    break;
                case PACKET3_3D_DRAW_INDX_2:
                    {
                        uint32_t ctrl = packets[i++];
                        uint32_t num_verts = ctrl >> 16;
                        count--;
                        if(ctrl & R200_VF_INDEX_SZ_4)
                        {
                            printf("Number of draw verticies 32bit %u\n", num_verts);
                            for(uint32_t j = 0; j < num_verts; j++)
                            {
                                i++;
                                count--;
                            }
                        }
                        else
                        {
                            num_verts /= 2;
                            printf("Number of draw verticies 16bit %u\n", num_verts);
                            for(uint32_t j = 0; j < num_verts; j++)
                            {
                                uint32_t vert = packets[i++];
                                printf("Vertex indices are %u %u\n", vert>>16, vert);
                                count--;
                            }
                        }
                    } 
                    break;
                default:
                    printf("Unknown opcode 0x%x\n", op_code);
                    break;
            }

            printf("Count is %ld\n", count);
            if(count <= 0)
            {
                packet_type = -1;
            }
        }
    }
}

void my_parse_relocs(uint32_t *packets, size_t num_packets, uint32_t *relocs, size_t num_relocs)
{
    for(size_t i=0; i < num_relocs/4; i++)
    {
        struct drm_radeon_cs_reloc *reloc = (struct drm_radeon_cs_reloc*)&relocs[i*4];
        printf("Reloc handle 0x%x\n", reloc->handle);
        printf("Reloc read domain 0x%x\n", reloc->read_domains);
        printf("Reloc write domain 0x%x\n", reloc->write_domain);
        printf("Reloc flag 0x%x\n", reloc->flags);
    }
}

void my_radeon_cs(struct drm_radeon_cs *cs)
{
    printf("num chunks %u\n", cs->num_chunks);
    printf("cs id %u\n", cs->cs_id);
    struct drm_radeon_cs_chunk **chunk_ptr = (struct drm_radeon_cs_chunk**)cs->chunks;
    printf("Chunks:\n");
    uint32_t *packets = NULL;
    size_t num_packets;
    uint32_t *relocs = NULL;
    size_t num_relocs;
    for(int i=0; i < (int)cs->num_chunks; i++)
    {
        printf("\t%u %u %llu\n", chunk_ptr[i][0].chunk_id, chunk_ptr[i][0].length_dw, chunk_ptr[i][0].chunk_data);
        if(chunk_ptr[i][0].chunk_id == RADEON_CHUNK_ID_IB)
        {
            printf("\tIB buffer\n");
            packets = (uint32_t*)chunk_ptr[i][0].chunk_data;
            num_packets = chunk_ptr[i][0].length_dw;
#if 0
            for(size_t j = 0; j < num_packets; j++)
            {
                printf("[0x%04lx] 0x%08x\n", j, packets[j]);
            }
#endif
        }
        else if(chunk_ptr[i][0].chunk_id == RADEON_CHUNK_ID_RELOCS)
        {
            printf("\treloc buffer\n");
            relocs = (uint32_t*)chunk_ptr[i][0].chunk_data;
            num_relocs = chunk_ptr[i][0].length_dw;
        }
    }

    if(relocs != NULL && packets != NULL)
        my_parse_relocs(packets, num_packets, relocs, num_relocs);

    if(packets != NULL)
        my_parse_packets(packets, num_packets);

    printf("gart limit %llu\n", cs->gart_limit);
    printf("vram limit %llu\n", cs->vram_limit);
    return;
}

void* mmap64(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    if(fd == dri_file)
    {
        return (void*)offset; 
    }
    else
    {
        return my_mmap64(addr, length, prot, flags, fd, offset);
    }
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    void *data = va_arg(args, void*);
    va_end(args);
    
    if(fd == dri_file)
    {
        int return_code = 0;
        switch(request)
        {
            case DRM_IOCTL_MODE_GETRESOURCES:
                printf("Got mode get resources\n");
                my_get_resources((struct drm_mode_card_res*)data);
                break;
            case DRM_IOCTL_MODE_GETCONNECTOR:
                printf("Mode connector\n");
                my_get_connector((struct drm_mode_get_connector*)data);
                break;
            case DRM_IOCTL_MODE_GETENCODER:
                printf("Get encoder\n");
                memcpy(data, &enc, sizeof(enc));
                break;
            case DRM_IOCTL_VERSION:
                printf("Get version\n");
                my_send_version((struct drm_version*)data);
                break;
            case DRM_IOCTL_RADEON_INFO:
                printf("Got radeon info\n");
                return_code = my_send_radeon_info((struct drm_radeon_info*)data);
                break;
            case DRM_IOCTL_RADEON_GEM_INFO:
                printf("Got GEM INFO\n");
                memcpy(data, &radeon_gem_info, sizeof(radeon_gem_info));
                break;
            case DRM_IOCTL_RADEON_GEM_CREATE:
                my_gem_create((struct drm_radeon_gem_create*)data);
                break;
            case DRM_IOCTL_GEM_FLINK:
                my_gem_flink((struct drm_gem_flink*)data);
                break;
            case DRM_IOCTL_RADEON_GEM_GET_TILING:
                memcpy(data, &get_tile_info, sizeof(get_tile_info));
                break;
            case DRM_IOCTL_RADEON_GEM_MMAP:
                my_gem_mmap((struct drm_radeon_gem_mmap*)data);
                break;
            case DRM_IOCTL_RADEON_GEM_WAIT_IDLE:
                // TODO figure out what we need to for for GEM wait idle
                return_code = 0;
                break;
            case DRM_IOCTL_RADEON_CS:
                my_radeon_cs((struct drm_radeon_cs*)data);
                return_code = -1; 
                break;
            default:
                printf("Got unknown ioctl 0x%08lx\n", request);
                return_code = -1;
                break;
        }

        return return_code;
    }
    else
    {
        return my_ioctl(fd, request, data);
    }
}

int open64(const char *path, int oflag, ...)
{
    printf("open called on %s\n", path);
    int file = my_open(path, oflag);

    string fpath(path);
    if(fpath == "/dev/dri/card0")
    {
        dri_file =  file;
    }

    return file;
}

int fstat64(int fd, struct stat *buf)
{
    printf("fstat called with %d\n", fd);

    if(fd == dri_file)
    {
        printf("Important file\n");
        my_fstat(fd, buf);

        //printf("device info 0x%04x 0x%04x\n", major(buf->st_rdev), minor(buf->st_rdev));
        return 0;
    }
    else
    {
        return my_fstat(fd, buf); 
    }
}

void my_lib_load()
{
    printf("Hello World\n");
    my_fstat = (my_fstat_t)dlsym(RTLD_NEXT, "fstat");
    my_open = (my_open_t)dlsym(RTLD_NEXT, "open");
    my_ioctl = (my_ioctl_t)dlsym(RTLD_NEXT, "ioctl");
    my_mmap64 = (my_mmap64_t)dlsym(RTLD_NEXT, "mmap");

    /* Init radeon data */
    res.count_fbs = 1;
    res.count_crtcs = 1;
    res.count_connectors = 1;
    res.count_encoders = 1;
    res.min_width = 0;
    res.min_height = 0;
    res.max_width = 640;
    res.max_height = 480;

    mode_info.hdisplay = 640;
    mode_info.vdisplay = 480;
    strcpy(mode_info.name, "my_disp");

    con_info.count_modes = 1;
    con_info.count_props = 1;
    con_info.count_encoders = 1;
    con_info.encoder_id = 1;
    con_info.connector_id = 1;
    con_info.connection = DRM_MODE_CONNECTED;

    version = {
        1, 4, 0,
        STR_SIZE("r200"),
        STR_SIZE(""),
        STR_SIZE("Emulated R200"),
    };
}
