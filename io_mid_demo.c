#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/genhd.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/random.h>

#define ZONE_SIZE (1ULL << 30)  // 1GB
#define NUM_ZONES 100

// 映射关系结构体
struct mapping_entry {
    int src_zone;
    int dest_zone;
};

// 区域结构体
struct zone {
    int zone_id;
    sector_t write_pointer;
};

// 块设备结构体
struct block_device *bdev;
sector_t device_size;

// 映射关系数组
struct mapping_entry mapping_table[NUM_ZONES];

// 区域数组
struct zone zones[NUM_ZONES];

// 中间层读写函数
void middle_layer_io(struct bio *bio)
{
    struct bio_vec bvec;
    struct bvec_iter iter;
    sector_t start_sector = bio->bi_iter.bi_sector;
    sector_t end_sector = start_sector + (bio_sectors(bio) - 1);
    int src_zone, dest_zone;
    struct bio *split_bio;

    // 检查LBA+size是否超出边界
    if (end_sector >= device_size)
    {
        bio_endio(bio, -EIO);
        return;
    }

    bio_for_each_segment(bvec, bio, iter)
    {
        sector_t sector = start_sector + (iter.bi_sector >> SECTOR_SHIFT);
        sector_t offset = sector % ZONE_SIZE;

        // 获取源区域和目标区域
        src_zone = sector / ZONE_SIZE;
        dest_zone = mapping_table[src_zone].dest_zone;

        // 检查是否跨越了zone
        if (offset + bvec.bv_len > ZONE_SIZE)
        {
            // 切割bio
            split_bio = bio_split(bio, offset, GFP_NOIO, &bio_set);
            if (split_bio)
            {
                // 修改切割后的bio的扇区地址
                split_bio->bi_iter.bi_sector = mapping_table[src_zone].dest_zone * ZONE_SIZE + (offset % ZONE_SIZE);
                // 下发切割后的bio
                submit_bio(split_bio);
            }
        }
        else
        {
            // 检查写指针是否匹配
            if (sector != zones[src_zone].write_pointer)
            {
                bio_endio(bio, -EIO);
                return;
            }

            // 修改扇区地址
            bio->bi_iter.bi_sector = dest_zone * ZONE_SIZE + offset;

            // 更新写指针
            zones[src_zone].write_pointer += bvec.bv_len >> SECTOR_SHIFT;
        }
    }

    // 传递IO请求给底层块设备
    submit_bio(bio);
}

// 完成处理切割的子bio
void end_bio(struct bio *bio)
{
    struct bio *parent_bio = bio->bi_private;

    // 释放子bio
    bio_put(bio);

    // 如果所有子bio都完成了，回调上层
    if (atomic_dec_and_test(&parent_bio->bi_remaining))
    {
        bio_endio(parent_bio, parent_bio->bi_status);
    }
}

// 初始化映射关系表和区域数组
void init_mapping_table_and_zones(void)
{
    int i;
    int dest_zones[NUM_ZONES];

    // 初始化目标区域数组
    for (i = 0; i < NUM_ZONES; i++)
    {
        dest_zones[i] = i;
    }

    // 随机打乱目标区域数组
    for (i = NUM_ZONES - 1; i > 0; i--)
    {
        int j = get_random_int() % (i + 1);
        int temp = dest_zones[i];
        dest_zones[i] = dest_zones[j];
        dest_zones[j] = temp;
    }

    // 构建映射关系表和区域数组
    for (i = 0; i < NUM_ZONES; i++)
    {
        mapping_table[i].src_zone = i;
        mapping_table[i].dest_zone = dest_zones[i];

        zones[i].zone_id = i;
        zones[i].write_pointer = 0;
    }
}

// 初始化函数
static int __init middle_layer_init(void)
{
    // 获取底层块设备
    bdev = blkdev_get_by_path("/dev/sda");
    if (!bdev)
    {
        printk(KERN_ERR "Failed to get block device\n");
        return -ENODEV;
    }

    // 获取设备大小
    device_size = i_size_read(bdev->bd_inode) >> SECTOR_SHIFT;

    // 初始化映射关系表和区域数组
    init_mapping_table_and_zones();

    // 注册中间层IO处理函数
    blk_queue_make_request(bdev_get_queue(bdev), middle_layer_io);

    printk(KERN_INFO "Middle layer initialized\n");
    return 0;
}

// 清理函数
static void __exit middle_layer_exit(void)
{
    // 释放块设备
    blkdev_put(bdev);

    printk(KERN_INFO "Middle layer exited\n");
}

module_init(middle_layer_init);
module_exit(middle_layer_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Zhao Jun");
MODULE_DESCRIPTION("IO Middle Layer");