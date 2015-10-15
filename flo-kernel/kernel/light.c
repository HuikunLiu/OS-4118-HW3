#include <linux/idr.h>
#include <linux/light.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

struct light_intensity intensity = { .cur_intensity = 0 };
int light_history[WINDOW], history_count = 0;

spinlock_t IDR_LOCK;

int has_events = 0;
DEFINE_IDR(events);
DECLARE_WAIT_QUEUE_HEAD(queue);

int event_check(int event_id)
{
	/* given a event_id, check if it's true based on the history */
	/* also return true if the event has been removed */
	int i, surpassed = 0;
	struct event_requirements *req;

	spin_lock(&IDR_LOCK);
	req = idr_find(&events, event_id);
	spin_unlock(&IDR_LOCK);
	if (req == NULL)
		return 1;

	/* TODO read lock*/
	for (i = 0; i < WINDOW && i < history_count; i++)
		if (light_history[i] > req->req_intensity - NOISE)
			surpassed++;

	return surpassed >= req->frequency;
}

/*
 * Set current ambient intensity in the kernel.
 *
 * The parameter user_light_intensity is the pointer to the address
 * where the sensor data is stored in user space. Follow system call
 * convention to return 0 on success and the appropriate error value
 * on failure.
 *
 * syscall number 378
 */
SYSCALL_DEFINE1(set_light_intensity, struct light_intensity __user *,
	user_light_intensity)
{
	if (copy_from_user(&intensity, user_light_intensity,
		sizeof(struct light_intensity)))
		return -EINVAL;
	return 0;
}

/*
 * Retrive the scaled intensity set in the kernel.
 *
 * The same convention as the previous system call but
 * you are reading the value that was just set.
 * Handle error cases appropriately and return values according to convention.
 * The calling process should provide memory in userspace to return the
 * intensity.
 *
 * syscall number 379
 */
SYSCALL_DEFINE1(get_light_intensity, struct light_intensity __user *,
	user_light_intensity)
{
	if (copy_to_user(user_light_intensity, &intensity,
		sizeof(struct light_intensity)))
		return -EINVAL;
	return 0;
}

/*
 * Create an event based on light intensity.
 *
 * If frequency exceeds WINDOW, cap it at WINDOW.
 * Return an event_id on success and the appropriate error on failure.
 *
 * system call number 380
 */
SYSCALL_DEFINE1(light_evt_create, struct event_requirements __user *,
	intensity_params)
{
	int event_id, ret;
	struct event_requirements *req;

	req = kmalloc(sizeof(struct event_requirements), GFP_KERNEL);
	if (req == NULL)
		return -EFAULT;

	if (copy_from_user(req, intensity_params,
			sizeof(struct event_requirements)))
		return -EINVAL;

	spin_lock(&IDR_LOCK);
	if (!has_events) {
		idr_init(&events);
		has_events = 1;
	}
	if (idr_pre_get(&events, GFP_KERNEL) == 0)
		return -ENOMEM;
	/* see LKD page 102 */
	do {
		if (!idr_pre_get(&events, GFP_KERNEL))
			return -ENOSPC;
		ret = idr_get_new(&events, req, &event_id);
	} while (ret == -EAGAIN);
	spin_unlock(&IDR_LOCK);

	return event_id;
}

/*
 * Block a process on an event.
 *
 * It takes the event_id as parameter. The event_id requires verification.
 * Return 0 on success and the appropriate error on failure.
 *
 * system call number 381
 */
SYSCALL_DEFINE1(light_evt_wait, int, event_id)
{
	/* see LKD page 59 */
	DEFINE_WAIT(wait);

	add_wait_queue(&queue, &wait);
	while (!event_check(event_id)) {
		prepare_to_wait(&queue, &wait, TASK_INTERRUPTIBLE);
		if (signal_pending(current))
			break;
		schedule();
	}
	finish_wait(&queue, &wait);

	return 0;
}

/*
 * The light_evt_signal system call.
 *
 * Takes sensor data from user, stores the data in the kernel,
 * and notifies all open events whose
 * baseline is surpassed.  All processes waiting on a given event
 * are unblocked.
 *
 * Return 0 success and the appropriate error on failure.
 *
 * system call number 382
 */
SYSCALL_DEFINE1(light_evt_signal, struct light_intensity __user *,
	user_light_intensity)
{
	struct light_intensity intensity;

	if (copy_from_user(&intensity, user_light_intensity,
			sizeof(struct light_intensity)))
		return -EINVAL;

	/* TODO write lock here */
	light_history[(history_count++) % WINDOW] = intensity.cur_intensity;
	wake_up(&queue);

	return 0;
}

/*
 * Destroy an event using the event_id.
 *
 * Return 0 on success and the appropriate error on failure.
 *
 * system call number 383
 */
SYSCALL_DEFINE1(light_evt_destroy, int, event_id)
{
	struct event_requirements *req;

	spin_lock(&IDR_LOCK);
	req = idr_find(&events, event_id);
	spin_unlock(&IDR_LOCK);

	if (req != NULL) {
		kfree(req);
		spin_lock(&IDR_LOCK);
		idr_remove(&events, event_id);
		spin_unlock(&IDR_LOCK);
	}
	return 0;
}
