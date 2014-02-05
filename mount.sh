sudo losetup -d /dev/loop0
sudo umount /mnt/osdrive
sudo losetup -o1048576  /dev/loop0 fs/c.img 
sudo mount -t ext2 /dev/loop0 /mnt/osdrive
