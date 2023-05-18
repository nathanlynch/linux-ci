// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "papr-vpd: " fmt

#include <linux/anon_inodes.h>
#include <linux/build_bug.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/seq_buf.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <asm/machdep.h>
#include <asm/papr-vpd.h>
#include <asm/rtas-work-area.h>
#include <asm/rtas.h>

/*
 * Internal VPD "blob" APIs: for accumulating successive ibm,get-vpd results
 * into a buffer to be attached to a file descriptor.
 */
struct vpd_blob {
	const char *data;
	size_t len;
};

static void vpd_blob_init(struct vpd_blob *blob, const char *data, size_t len)
{
	*blob = (struct vpd_blob){
		.data = data,
		.len = len,
	};
}

static struct vpd_blob *vpd_blob_new(void)
{
	struct vpd_blob *blob = kmalloc(sizeof(struct vpd_blob), GFP_KERNEL);

	if (blob)
		vpd_blob_init(blob, NULL, 0);
	return blob;
}

static void vpd_blob_free(struct vpd_blob *blob)
{
	if (blob) {
		kvfree(blob->data);
		kfree(blob);
	}
}

static bool vpd_blob_has_data(const struct vpd_blob *blob)
{
	WARN_ON_ONCE(blob->data && blob->len == 0);
	WARN_ON_ONCE(!blob->data && blob->len != 0);
	return !!blob->data;
}

static size_t vpd_blob_detach_data(struct vpd_blob *blob, char **buf)
{
	const size_t len = blob->len;

	*buf = (char *)blob->data;
	vpd_blob_init(blob, NULL, 0);

	return len;
}

/*
 * For when the blob has no data attached yet.
 */
static int vpd_blob_setup(struct vpd_blob *blob, const char *data, size_t len)
{
	char *ptr;

	ptr = kvmalloc(len, GFP_KERNEL);
	if (!ptr)
		return -ENOMEM;

	memcpy(ptr, data, len);
	vpd_blob_init(blob, ptr, len);
	return 0;
}

/*
 * For when the blob already has data attached.
 */
static int vpd_blob_append(struct vpd_blob *blob, const char *data, size_t len)
{
	const size_t new_len = blob->len + len;
	const size_t old_len = blob->len;
	const char *old_ptr = blob->data;
	char *new_ptr;

	if (!vpd_blob_has_data(blob))
		return vpd_blob_setup(blob, data, len);

	new_ptr = kvrealloc(old_ptr, old_len, new_len, GFP_KERNEL);
	if (!new_ptr)
		return -ENOMEM;

	memcpy(&new_ptr[old_len], data, len);
	blob->data = new_ptr;
	blob->len = new_len;
	return 0;
}

/**
 * struct rtas_ibm_get_vpd_params - Parameters (in and out) for ibm,get-vpd.
 *
 * @loc_code: In: Location code buffer. Must be RTAS-addressable.
 * @work_area: In: Work area buffer for results.
 * @sequence: In: Sequence number. Out: Next sequence number.
 * @written: Out: Bytes written by ibm,get-vpd to @work_area.
 * @status: Out: RTAS call status.
 */
struct rtas_ibm_get_vpd_params {
	const struct papr_location_code *loc_code;
	struct rtas_work_area *work_area;
	u32 sequence;
	u32 written;
	s32 status;
};

static int rtas_ibm_get_vpd(struct rtas_ibm_get_vpd_params *params)
{
	const struct papr_location_code *loc_code = params->loc_code;
	struct rtas_work_area *work_area = params->work_area;
	u32 rets[2];
	s32 fwrc;
	int ret;

	pr_debug("%s entry: params = { .seq=%u, .written=%u, .status=%d }\n", __func__,
		 params->sequence, params->written, params->status);

	do {
		fwrc = rtas_call(rtas_function_token(RTAS_FN_IBM_GET_VPD), 4, 3,
				 rets,
				 __pa(loc_code),
				 rtas_work_area_phys(work_area),
				 rtas_work_area_size(work_area),
				 params->sequence);
	} while (rtas_busy_delay(fwrc));

	switch (fwrc) {
	case -1:
		ret = -EIO;
		break;
	case -3:
		ret = -EINVAL;
		break;
	case -4:
		ret = -EAGAIN;
		break;
	case 1:
		params->sequence = rets[0];
		fallthrough;
	case 0:
		params->written = rets[1];
		/*
		 * Kernel or firmware bug, do not continue.
		 */
		if (WARN(params->written > rtas_work_area_size(work_area),
			 "possible write beyond end of work area"))
			ret = -EFAULT;
		else
			ret = 0;
		break;
	default:
		ret = -EIO;
		pr_err_ratelimited("unexpected ibm,get-vpd status %d\n", fwrc);
		break;
	}

	params->status = fwrc;

	pr_debug("%s exit: ret = %d, params = { .seq=%u, .written=%u, .status=%d }\n", __func__,
		 ret, params->sequence, params->written, params->status);

	return ret;
}

struct vpd_sequence_state {
	struct mutex *mutex;
	struct pin_cookie cookie;
	int error;
	struct rtas_ibm_get_vpd_params params;
};

static void vpd_sequence_begin(struct vpd_sequence_state *state, const struct papr_location_code *loc_code)
{
	static DEFINE_MUTEX(vpd_sequence_mutex);
	/*
	 * Use a static data structure for the location code passed to
	 * RTAS to ensure it's in the RMA and avoid a separate work
	 * area allocation.
	 */
	static struct papr_location_code static_loc_code;

	mutex_lock(&vpd_sequence_mutex);

	static_loc_code = *loc_code;
	*state = (struct vpd_sequence_state) {
		.mutex = &vpd_sequence_mutex,
		.cookie = lockdep_pin_lock(&vpd_sequence_mutex),
		.params = {
			.work_area = rtas_work_area_alloc(SZ_4K),
			.loc_code = &static_loc_code,
			.sequence = 1,
		},
	};
}

static bool vpd_sequence_done(const struct vpd_sequence_state *state)
{
	bool done;

	if (state->error)
		return true;

	switch (state->params.status) {
	case 0:
		if (state->params.written == 0)
			done = false; /* Initial state. */
		else
			done = true; /* All data consumed. */
		break;
	case 1:
		done = false; /* More data available. */
		break;
	default:
		done = true; /* Error encountered. */
		break;
	}

	return done;
}

static bool vpd_sequence_advance(struct vpd_sequence_state *state)
{
	if (vpd_sequence_done(state))
		return false;

	state->error = rtas_ibm_get_vpd(&state->params);

	return state->error == 0;
}

static size_t vpd_sequence_get_buffer(const struct vpd_sequence_state *state, const char **buf)
{
	*buf = rtas_work_area_raw_buf(state->params.work_area);
	return state->params.written;
}

static void vpd_sequence_set_err(struct vpd_sequence_state *state, int err)
{
	state->error = err;
}

static void vpd_sequence_end(struct vpd_sequence_state *state)
{
	rtas_work_area_free(state->params.work_area);
	lockdep_unpin_lock(state->mutex, state->cookie);
	mutex_unlock(state->mutex);
}

/*
 * Given the location code, initialize the provided seq_buf with the
 * corresponding VPD resulting from a complete ibm,get-vpd call
 * sequence.
 */
static int papr_vpd_retrieve(const struct papr_location_code *loc_code, struct seq_buf *seq)
{
	struct vpd_sequence_state state;
	struct vpd_blob *blob;

	blob = vpd_blob_new();
	if (!blob)
		return -ENOMEM;

	vpd_sequence_begin(&state, loc_code);

	while (vpd_sequence_advance(&state)) {
		const char *buf;
		const size_t len = vpd_sequence_get_buffer(&state, &buf);

		vpd_sequence_set_err(&state, vpd_blob_append(blob, buf, len));
	}

	vpd_sequence_end(&state);

	if (!state.error) {
		char *buf;
		size_t len = vpd_blob_detach_data(blob, &buf);

		seq_buf_init(seq, buf, len);
		seq_buf_commit(seq, len);
	}

	vpd_blob_free(blob);

	return state.error;
}

static bool papr_location_code_is_terminated(const struct papr_location_code *lc)
{
	return string_is_terminated(lc->str, ARRAY_SIZE(lc->str));
}

static bool handle_valid(const struct papr_vpd_handle *handle)
{
	if (!papr_location_code_is_terminated(&handle->loc_code))
		return false;

	for (size_t i = 0; i < ARRAY_SIZE(handle->reserved); ++i) {
		if (handle->reserved[i] != 0)
			return false;
	}

	return true;
}

static ssize_t papr_vpd_handle_read(struct file *file, char __user *buf, size_t size, loff_t *off)
{
	struct seq_buf *vpd = file->private_data;
	const size_t copy_size = min_t(size_t, size, seq_buf_used(vpd));
	const ssize_t ret = seq_buf_to_user(vpd, buf, copy_size);

	/* convert -EBUSY to EOF */
	return ret == -EBUSY ? 0 : ret;
}

static int papr_vpd_handle_release(struct inode *inode, struct file *file)
{
	struct seq_buf *vpd = file->private_data;

	kvfree(vpd->buffer);
	kfree(vpd);

	return 0;
}

static struct file_operations papr_vpd_handle_ops = {
	.read = papr_vpd_handle_read,
	.release = papr_vpd_handle_release,
};

/*
 * Happy path:
 * - copy handle object in from user
 * - retrieve VPD for loc code into vpd buffer object
 * - get unused fd - read-only+cloexec
 * - create anonymous struct file, attaching vpd buffer
 * - copy handle object back to user - need to communicate fd and
 *   buffer size, and I don't think fstat() -> stat.st_size will work
 *   here.
 * - fd_install(fd, file) - must be last, cannot unwind
 */
static long papr_vpd_ioctl_create_handle(struct papr_vpd_handle __user *uhandle)
{
	struct papr_vpd_handle *khandle;
	struct seq_buf *vpd;
	struct file *file;
	long ret;
	int fd;

	khandle = memdup_user(uhandle, sizeof(*khandle));
	if (IS_ERR(khandle))
		return PTR_ERR(khandle);

	if (!handle_valid(khandle)) {
		ret = -EINVAL;
		goto free_khandle;
	}

	vpd = kzalloc(sizeof(*vpd), GFP_KERNEL);
	if (!vpd) {
		ret = -ENOMEM;
		goto free_khandle;
	}

	ret = papr_vpd_retrieve(&khandle->loc_code, vpd);
	if (ret)
		goto free_vpd;

	fd = get_unused_fd_flags(O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		ret = fd;
		goto free_vpd;
	}

	khandle->size = seq_buf_used(vpd);
	khandle->fd = fd;

	if (copy_to_user(uhandle, khandle, sizeof(*khandle))) {
		ret = -EFAULT;
		goto put_fd;
	}

	file = anon_inode_getfile("[papr-vpd]", &papr_vpd_handle_ops,
				  vpd, O_RDONLY);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto put_fd;
	}

	fd_install(fd, file);
	kfree(khandle);
	return 0;
put_fd:
	put_unused_fd(fd);
free_vpd:
	kfree(vpd); // fixme: need to free vpd->buf!
free_khandle:
	kfree(khandle);
	return ret;
}

/* handler for /dev/papr-vpd */
static long papr_vpd_dev_ioctl(struct file *filp, unsigned int ioctl, unsigned long arg)
{
	void __user *argp = (__force void __user *)arg;
	long ret;

	switch (ioctl) {
	case PAPR_VPD_CREATE_HANDLE:
		ret = papr_vpd_ioctl_create_handle(argp);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}
	return ret;
}

static const struct file_operations papr_vpd_ops = {
	.unlocked_ioctl = papr_vpd_dev_ioctl,
	// todo: implement .poll for VPD change post-LPM
};

static struct miscdevice papr_vpd_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "papr-vpd",
	.fops = &papr_vpd_ops,
};

static __init int papr_vpd_init(void)
{
	if (!rtas_function_implemented(RTAS_FN_IBM_GET_VPD))
		return -ENODEV;

	return misc_register(&papr_vpd_dev);
}
machine_device_initcall(pseries, papr_vpd_init);
