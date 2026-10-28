/* Guest-only end-to-end readahead and clean-fsync/group commit checks. */
static int guest_write_sectors(unsigned long long *out)
{
	FILE *f=fopen("/sys/block/vda/stat","r");
	unsigned long long x[7];
	int ok;
	if(!f)return -1;
	ok=fscanf(f,"%llu %llu %llu %llu %llu %llu %llu",
	          &x[0],&x[1],&x[2],&x[3],&x[4],&x[5],&x[6]);
	fclose(f);
	if(ok!=7)return -1;
	*out=x[6]; return 0;
}
static void human_readahead_group_test(void)
{
	int fd=-1;
	unsigned char bytes[4096];
	unsigned long long a=0,b=0;
	size_t block,i;
	fd=open("/mnt/human/ra-sequential.bin",O_RDWR|O_CREAT|O_EXCL,0644);
	if(fd<0){human_report("DEFECT","readahead_open",errno);return;}
	for(block=0;block<64;block++){
		for(i=0;i<sizeof(bytes);i++)
			bytes[i]=(unsigned char)((block*197U+i*71U+31U)&255U);
		if(write(fd,bytes,sizeof(bytes))!=(ssize_t)sizeof(bytes)){
			human_report("DEFECT","readahead_write",errno);goto done;
		}
	}
	if(fsync(fd)){human_report("DEFECT","readahead_fsync",errno);goto done;}
	if(guest_write_sectors(&a)){
		human_report("MISSING","block_stats_for_group_commit",errno);
	}else{
		if(fsync(fd)||guest_write_sectors(&b))
			human_report("DEFECT","group_fsync_second",errno);
		else if(a!=b)
			human_report("DEFECT","group_fsync_wrote_clean_file",(int)(b-a));
		else
			msg("VTXFS_VM_HUMAN_OK item=fsync_clean_no_extra_block_writes\n");
	}
	if(posix_fadvise(fd,0,64*4096,POSIX_FADV_DONTNEED))
		human_report("MISSING","posix_fadvise_dontneed",0);
	if(lseek(fd,0,SEEK_SET)!=0){
		human_report("DEFECT","readahead_seek",errno);goto done;
	}
	for(block=0;block<64;block++){
		if(read(fd,bytes,sizeof(bytes))!=(ssize_t)sizeof(bytes)){
			human_report("DEFECT","readahead_read",errno);goto done;
		}
		for(i=0;i<sizeof(bytes);i++)
			if(bytes[i]!=(unsigned char)((block*197U+i*71U+31U)&255U)){
				human_report("DEFECT","readahead_crc_data",0);goto done;
			}
	}
	msg("VTXFS_VM_HUMAN_OK item=sequential_cold_256KiB\n");
done:
	close(fd);
}
