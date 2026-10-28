// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sysfs root is shared by ALL mounted VTXFS volumes, per-volume attributes
 * are under unique mount-instance names, including when fsid UUIDs match.
 */
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/atomic.h>

#include "vtxfs_subsys.h"
#include "vtxfs_trace.h"

struct vtxfs_sysfs {
	struct kobject *fsobj;
	struct vtxfs_fs_info *fs;
	struct kobj_attribute label_attr;
	struct kobj_attribute features_attr;
};

static DEFINE_MUTEX(vtxfs_sysfs_lock);
static struct kobject *vtxfs_sysfs_root;
static unsigned int vtxfs_sysfs_mounts;
static atomic_t vtxfs_sysfs_serial = ATOMIC_INIT(0);

static ssize_t label_show(struct kobject *kobj, struct kobj_attribute *a,
			  char *buf)
{
	struct vtxfs_sysfs *s = container_of(a, struct vtxfs_sysfs, label_attr);

	(void)kobj;
	return sysfs_emit(buf, "%.*s\n", VTXFS_LABEL_SIZE, s->fs->super.label);
}

static ssize_t features_show(struct kobject *kobj, struct kobj_attribute *a,
			     char *buf)
{
	struct vtxfs_sysfs *s = container_of(a, struct vtxfs_sysfs, features_attr);

	(void)kobj;
	return sysfs_emit(buf, "0x%llx\n",
			  (unsigned long long)vtx_cpu64(
				  s->fs->super.incompat_flags));
}

int vtxfs_sysfs_init(struct vtxfs_fs_info *fs)
{
	struct vtxfs_sysfs *s;
	char unique[56];
	int ret = 0;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->fs = fs;

	mutex_lock(&vtxfs_sysfs_lock);
	if (!vtxfs_sysfs_root) {
		vtxfs_sysfs_root = kobject_create_and_add("vtxfs", fs_kobj);
		if (!vtxfs_sysfs_root) {
			ret = -ENOMEM;
			goto unlock_free;
		}
	}
	/* A uuid is NOT unique among cloned images. Use a mount serial to
	 * avoid -EEXIST and a kernel stack trace for the second volume. */
	snprintf(unique, sizeof(unique), "%pU-%u", fs->super.fsid_uuid,
		 (unsigned int)atomic_inc_return(&vtxfs_sysfs_serial));
	s->fsobj = kobject_create_and_add(unique, vtxfs_sysfs_root);
	if (!s->fsobj) {
		ret = -ENOMEM;
		goto unlock_drop_root;
	}

	sysfs_attr_init(&s->label_attr.attr);
	s->label_attr.attr.name = "label";
	s->label_attr.attr.mode = 0444;
	s->label_attr.show = label_show;
	sysfs_attr_init(&s->features_attr.attr);
	s->features_attr.attr.name = "features";
	s->features_attr.attr.mode = 0444;
	s->features_attr.show = features_show;

	ret = sysfs_create_file(s->fsobj, &s->label_attr.attr);
	if (ret)
		goto unlock_drop_obj;
	ret = sysfs_create_file(s->fsobj, &s->features_attr.attr);
	if (ret) {
		sysfs_remove_file(s->fsobj, &s->label_attr.attr);
		goto unlock_drop_obj;
	}
	vtxfs_sysfs_mounts++;
	fs->sysfs_priv = s;
	mutex_unlock(&vtxfs_sysfs_lock);
	return 0;

unlock_drop_obj:
	kobject_put(s->fsobj);
unlock_drop_root:
	if (vtxfs_sysfs_mounts == 0) {
		kobject_put(vtxfs_sysfs_root);
		vtxfs_sysfs_root = NULL;
	}
unlock_free:
	mutex_unlock(&vtxfs_sysfs_lock);
	kfree(s);
	return ret;
}

void vtxfs_sysfs_exit(struct vtxfs_fs_info *fs)
{
	struct vtxfs_sysfs *s;

	if (!fs)
		return;
	mutex_lock(&vtxfs_sysfs_lock);
	s = fs->sysfs_priv;
	if (s) {
		sysfs_remove_file(s->fsobj, &s->features_attr.attr);
		sysfs_remove_file(s->fsobj, &s->label_attr.attr);
		kobject_put(s->fsobj);
		fs->sysfs_priv = NULL;
		kfree(s);
		if (--vtxfs_sysfs_mounts == 0) {
			kobject_put(vtxfs_sysfs_root);
			vtxfs_sysfs_root = NULL;
		}
	}
	mutex_unlock(&vtxfs_sysfs_lock);
}
