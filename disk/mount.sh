sudo losetup -d /dev/loop0
sudo umount /mnt/osdrive
sudo losetup  /dev/loop0 hdd.img 
sudo mount -t ext2 /dev/loop0 /mnt/osdrive
