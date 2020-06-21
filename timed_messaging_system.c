#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/list.h> 
#include <linux/version.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include "timed_messaging_system.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tommaso Villa");
MODULE_DESCRIPTION("The module allows the communication across thread through a timed messaging system");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
#define get_major(session)	MAJOR(session->f_inode->i_rdev)
#define get_minor(session)	MINOR(session->f_inode->i_rdev)
#else
#define get_major(session)	MAJOR(session->f_dentry->d_inode->i_rdev)
#define get_minor(session)	MINOR(session->f_dentry->d_inode->i_rdev)
#endif


//The manipulation of module parameters is allowed only for owner user and for the user group he belongs to.
static int max_message_size = DEFAULT_MAX_MESSAGE_SIZE;
module_param(max_message_size, int, 0660);
MODULE_PARM_DESC(max_message_size, "The maximum size for a message posted on a device file");

static int max_storage_size = DEFAULT_MAX_STORAGE_SIZE;
module_param(max_storage_size, int, 0660);
MODULE_PARM_DESC(max_storage_size, "The maximum storage size for all messages posted on a device file");


static int major_number; 
static struct minor minors[MAX_MINOR_NUMBER];


static int dev_open(struct inode *inode, struct file *file) {

	struct session *new_session;
	int minor_number = get_minor(file);

	//A new session for the given minor is created and linked to the others
	new_session = kmalloc(sizeof(struct session), GFP_KERNEL);
	new_session->send_timeout = DEFAULT_SEND_TIMEOUT;
	new_session->recv_timeout = DEFAULT_RECV_TIMEOUT;
	new_session->workqueue = alloc_workqueue("pending_writes", WQ_MEM_RECLAIM, 0);
	mutex_init(&(new_session->session_mutex));
	INIT_LIST_HEAD(&new_session->list);
	INIT_LIST_HEAD(&new_session->pending_writes);

	mutex_lock(&(minors[minor_number].operation_synchronizer));
	list_add(&new_session->list, &minors[minor_number].sessions);
  	mutex_unlock(&(minors[minor_number].operation_synchronizer));

	//The pointer to the just created session is stored in private_data field of file struct
	file->private_data = new_session;

	AUDIT
	printk("%s: Open on device [%d,%d]\n", MODULE_NAME, major_number, minor_number);

	return 0;
}


static int dev_release(struct inode *inode, struct file *file) {

 	struct session *current_session;
	int minor_number = get_minor(file);

	current_session = (struct session*)(file->private_data);

	//The session to close is removed from the list of sessions of device file
	mutex_lock(&(minors[minor_number].operation_synchronizer));
	list_del(&(current_session->list));
	mutex_unlock(&(minors[minor_number].operation_synchronizer));

	//The completation of works in the workqueue is waited before to destroy the workqueue and to deallocate the session. 
	//Warning: the works inserted in the workqueue after the invocation of these function will be not executed
	flush_workqueue(current_session->workqueue);
	destroy_workqueue(current_session->workqueue);
	kfree(current_session);

	AUDIT
	printk("%s: Close on device [%d,%d]\n", MODULE_NAME, major_number, minor_number);

	return 0;
}


//enqueue_message is the function called when the timer for a delayed write expires. Using the pointer to work_struct is possible obtain
//the delayed_work that contains the work_struct and the pending_write that contains the delayed_work.
static void enqueue_message(struct work_struct *work){

	struct pending_write *pending_write;
	struct delayed_work *delayed_work;
	struct message *new_message;
	int minor_number;

	delayed_work = container_of(work, struct delayed_work, work);
	pending_write = container_of(delayed_work, struct pending_write, delayed_work);
	new_message = &(pending_write->message);
	minor_number = pending_write->minor_number;

	//The message is effectly posted, the number of available readings is updated and eventually a sleeping reader is awaked 
	mutex_lock(&(minors[minor_number].operation_synchronizer));
	list_add(&(new_message->list), &(minors[minor_number].messages));
	minors[minor_number].available_readings += 1;
	wake_up(&(minors[minor_number].pending_readers_wq));
	mutex_unlock(&(minors[minor_number].operation_synchronizer));

	AUDIT
	printk("%s: Deferred write completed on device [%d,%d]\n", MODULE_NAME, major_number, minor_number);

	return;	
}


static ssize_t dev_write(struct file *file, const char *buff, size_t len, loff_t *off) {

	struct session *current_session;
	struct message *new_message;
	struct pending_write *pending_write;
	int minor_number = get_minor(file);
	int unwritten_chars;
	int send_timeout;

	AUDIT
	printk("%s: Write called on device [%d,%d]\n", MODULE_NAME, major_number, minor_number);

	//Check if the size of message is too large
	if (len > max_message_size) {
		AUDIT
		printk("%s: Write aborted on device [%d,%d]: too long message\n", MODULE_NAME, major_number, minor_number);
		return -1;	
	}		

	//Check if the total size of messages in the device file is too large
	mutex_lock(&(minors[minor_number].operation_synchronizer));
	if (minors[minor_number].storage_size + len > max_storage_size){
		mutex_unlock(&(minors[minor_number].operation_synchronizer));
		AUDIT
		printk("%s: Write aborted on device [%d,%d]: not enough space for storing message\n", MODULE_NAME, major_number, minor_number);
		return -1;	
	}

	//If the write can occur, the storage size of the device file is updated
	minors[minor_number].storage_size = minors[minor_number].storage_size + len;
	mutex_unlock(&(minors[minor_number].operation_synchronizer));

	//The new message is created 
	new_message = kmalloc(sizeof(struct message), GFP_KERNEL);
	new_message->text = kmalloc(len, GFP_KERNEL);
	new_message->size = len;
	new_message->is_delayed = false;
	unwritten_chars = copy_from_user(new_message->text, buff, len);
	INIT_LIST_HEAD(&(new_message->list));

	//Checking if the message has to be immediatly posted or not
	current_session = (struct session*)(file->private_data);
	mutex_lock(&(current_session->session_mutex));
	send_timeout = current_session->send_timeout;
	mutex_unlock(&(current_session->session_mutex));

	if (send_timeout == 0){

		//The message is immediatly posted, the number of available readings is updated and eventually a sleeping reader is awaked
		mutex_lock(&(minors[minor_number].operation_synchronizer));
		list_add(&(new_message->list), &(minors[minor_number].messages));
		minors[minor_number].available_readings += 1;
		wake_up(&(minors[minor_number].pending_readers_wq));
		mutex_unlock(&(minors[minor_number].operation_synchronizer));

		AUDIT
		printk("%s: Write done on device [%d,%d]\n", MODULE_NAME, major_number, minor_number);

		return len - unwritten_chars;

	} else {

		//The message posting is deferred
		pending_write = kmalloc(sizeof(struct pending_write), GFP_KERNEL);
		pending_write->minor_number = minor_number;
		INIT_LIST_HEAD(&(pending_write->list));

		//The message is copied into a pending_message struct, so the original message can be deallocated
		//NB: the field text of message can't be deallocated until the read occurs
		new_message->is_delayed = true;
		pending_write->message = *(new_message);
		kfree(new_message);		

		INIT_DELAYED_WORK(&(pending_write->delayed_work), enqueue_message);

		mutex_lock(&(current_session->session_mutex));
		list_add(&(pending_write->list), &(current_session->pending_writes));
		queue_delayed_work(current_session->workqueue, &(pending_write->delayed_work), current_session->send_timeout);
		mutex_unlock(&(current_session->session_mutex));
	
		AUDIT
		printk("%s: Write deferred on device [%d,%d]\n", MODULE_NAME, major_number, minor_number);

		return 0;
	}

}

static ssize_t dev_read(struct file *file, char *buff, size_t len, loff_t *off) {

	struct pending_read *pending_read = NULL;
	struct session *current_session;
	struct message *message_to_read;
	int unread_chars;
	int minor_number = get_minor(file);
	long recv_timeout;
	int wait_outcome;

	AUDIT
	printk("%s: Read called on device [%d,%d]\n", MODULE_NAME, major_number, minor_number);

	//Checking if the read has to be blocking or not
	current_session = (struct session*)(file->private_data);
	mutex_lock(&(current_session->session_mutex));
	recv_timeout = current_session->recv_timeout;
	mutex_unlock(&(current_session->session_mutex));

	//Checking if on the device there are available messages
	mutex_lock(&(minors[minor_number].operation_synchronizer));
	if (minors[minor_number].available_readings == 0){
		mutex_unlock(&(minors[minor_number].operation_synchronizer));

		if (recv_timeout == 0) {
			//Non blocking read

			AUDIT
			printk("%s: Read aborted on device [%d,%d]: not messages to read\n", MODULE_NAME, major_number, minor_number);
			return -1;

		} else {
			//Blocking read
		
			//A pending_reading struct is allocated and linked whit the other pending readings for the device file.
			//That list will be used in case of invocation of dev_flush()
			pending_read = kmalloc(sizeof(struct pending_read), GFP_KERNEL);
			pending_read -> is_flushed = false;
			INIT_LIST_HEAD(&(pending_read->list));

			mutex_lock(&(minors[minor_number].operation_synchronizer));
			list_add(&(pending_read->list), &(minors[minor_number].pending_readings));
			mutex_unlock(&(minors[minor_number].operation_synchronizer));


			while(true){
				//The thread sleeps on the waitqueue associated whit the device file.
				//It can be woken up if the condition becames true or timeout expires
				wait_outcome = wait_event_timeout(minors[minor_number].pending_readers_wq,
									minors[minor_number].available_readings > 0 || pending_read->is_flushed, recv_timeout);
				
				if (wait_outcome == 0){
					//Timer expired before wait condition changes: the reading is canceled

					mutex_lock(&(minors[minor_number].operation_synchronizer));
					list_del(&(pending_read->list));
					mutex_unlock(&(minors[minor_number].operation_synchronizer));
					kfree(pending_read);

					AUDIT
					printk("%s: Read aborted on device [%d,%d]: not messages to read after timeout expiration\n", 
						MODULE_NAME, major_number, minor_number);
					return -1;
				} 
				
				//If the thread reachs this point of code it means the condition changes before timer expiration.
				//However, to prevent race condition due to concurring awakes, the condition is checked again

				if (pending_read->is_flushed){
					//Another thread invoked flush on the device file

					mutex_lock(&(minors[minor_number].operation_synchronizer));
					list_del(&(pending_read->list));
					mutex_unlock(&(minors[minor_number].operation_synchronizer));
					kfree(pending_read);

					AUDIT
					printk("%s: Read aborted on device [%d,%d]: another process calls flush()\n", 
						MODULE_NAME, major_number, minor_number);
					return -1;
				}

				mutex_lock(&(minors[minor_number].operation_synchronizer));	

				if (minors[minor_number].available_readings == 0){
					mutex_unlock(&(minors[minor_number].operation_synchronizer));
					//The condition is evalued as false again. The wait_event_timeout returns the residual jiffies so the
					//the thread returns to sleep but whit a different timeout				
					recv_timeout = recv_timeout - wait_outcome;

				} else {
					//The condition is verified, so the reading can occur. 
					//NB: here the thread still has the lock on device file
					break;
				}
			}
		}			
	}

	minors[minor_number].available_readings -= 1;
	
	//The pending_read struct is removed from the list in the device file and it is deallocated
	if (pending_read != NULL) {
		list_del(&(pending_read->list));
		kfree(pending_read);
	}

	if (minors[minor_number].message_to_read == NULL){
	//The read operation is invoked for the first time or after the message list has been emptied. In the message list new message are 
	//always inserted after the head. So in this case the message to read is the previous respect the head.
		minors[minor_number].message_to_read = minors[minor_number].messages.prev;	
	}

	//The read occurs here: the pointer to next message to read is updated and the total size of storage for device file is updated too
	message_to_read = list_entry(minors[minor_number].message_to_read, struct message, list);
	if (message_to_read->size < len){
		len = strlen(message_to_read->text);	
	}
	unread_chars = copy_to_user(buff, message_to_read->text, len);
	minors[minor_number].message_to_read = message_to_read->list.prev;
	minors[minor_number].storage_size = minors[minor_number].storage_size - message_to_read->size;
	list_del(&(message_to_read->list));

	if (list_empty(&(minors[minor_number].messages))){
		minors[minor_number].message_to_read = NULL;
	}

	mutex_unlock(&(minors[minor_number].operation_synchronizer));

	
	//If the message was delayed the pending_message struct is deallocated, otherwise the message is deallocated
	if (message_to_read->is_delayed){
		struct pending_write *pending_write_to_delete;
		pending_write_to_delete = container_of(message_to_read, struct pending_write, message);

		//Pending write struct is also removed from the list associated whit the session
		mutex_lock(&(current_session->session_mutex));
		list_del(&(pending_write_to_delete->list));
		mutex_unlock(&(current_session->session_mutex));
	
		kfree(pending_write_to_delete->message.text);
		kfree(pending_write_to_delete);

	} else {
		kfree(message_to_read->text);
		kfree(message_to_read);
	}

	AUDIT	
	printk("%s: Read done on device [%d,%d]\n", MODULE_NAME, major_number, minor_number);

	return len - unread_chars;
}

static long dev_ioctl(struct file *file, unsigned int command, unsigned long param) {

	int minor_number = get_minor(file);
	struct session *current_session;
	struct list_head *position;
	struct list_head *temp_position;
	struct pending_write *pending_write;
	size_t storage_freed = 0;

	AUDIT
	printk("%s: Ioctl called on device [%d,%d] with command %d\n", MODULE_NAME, major_number, minor_number, command);

	current_session = (struct session*)(file->private_data);
	mutex_lock(&(current_session->session_mutex));

	switch(command){

		case SET_SEND_TIMEOUT:
			current_session->send_timeout = (long)param;
			mutex_unlock(&(current_session->session_mutex));
			break;
		
		case SET_RECV_TIMEOUT:
			current_session->recv_timeout = (long)param;
			mutex_unlock(&(current_session->session_mutex));
			break;

		case REVOKE_DELAYED_MESSAGES:		

			//The pending writes on the current session are canceled and the structs deallocated
			list_for_each_safe(position, temp_position, &(current_session->pending_writes)) {
				
				pending_write = list_entry(position, struct pending_write, list);
				if (cancel_delayed_work(&(pending_write->delayed_work))) {

					list_del(&(pending_write->list));
					storage_freed += pending_write->message.size;
					kfree(pending_write->message.text);
					kfree(pending_write);

					AUDIT					
					printk("%s: Deferred write canceled on device [%d,%d]\n", MODULE_NAME, major_number, minor_number);
				}
			}

			mutex_unlock(&(current_session->session_mutex));
			
			//The total storage size for current minor is updated after cancellation of pending writes 
			mutex_lock(&(minors[minor_number].operation_synchronizer));		
			minors[minor_number].storage_size = minors[minor_number].storage_size - storage_freed;
			mutex_unlock(&(minors[minor_number].operation_synchronizer));		
			break;

		default:
			mutex_unlock(&(current_session->session_mutex));
			break;
	}

	return 0;
}


static int dev_flush(struct file *file, fl_owner_t id) {
	struct list_head *pos_i;
	struct list_head *pos_j;
	struct list_head *temp_pos;
	struct pending_read *pending_read;
	struct session *session;
	struct pending_write *pending_write;
	int minor_number = get_minor(file);
	size_t storage_freed = 0;
	
	AUDIT
	printk("%s: Flush called on device [%d,%d]\n", MODULE_NAME, major_number, minor_number);

	//Acquiring lock for device file
	mutex_lock(&(minors[minor_number].operation_synchronizer));

	//Canceling each delayed message through the sessions opened for the device file
	list_for_each(pos_i, &minors[minor_number].sessions) { 
	    session = list_entry(pos_i, struct session, list); 	

		//Acquiring lock for session
		mutex_lock(&(session->session_mutex));

		list_for_each_safe(pos_j, temp_pos, &(session->pending_writes)) {

				pending_write = list_entry(pos_j, struct pending_write, list);
				//The canceling of delayed work can fail if the task is already started
				if (cancel_delayed_work(&(pending_write->delayed_work))) {

					list_del(&(pending_write->list));
					storage_freed += pending_write->message.size;
					kfree(pending_write->message.text);
					kfree(pending_write);

					AUDIT					
					printk("%s: Deferred write canceled on device [%d,%d]\n", MODULE_NAME, major_number, minor_number);
				}
			}

		mutex_unlock(&(session->session_mutex));
	}

	minors[minor_number].storage_size = minors[minor_number].storage_size - storage_freed;

	//Canceling each blocked reading on the device file marking the apposite flag in the pending_read struct
	list_for_each(pos_i, &minors[minor_number].pending_readings) { 
    	pending_read = list_entry(pos_i, struct pending_read, list); 	
		pending_read->is_flushed = true;	 
    }
	wake_up_all(&(minors[minor_number].pending_readers_wq));

	mutex_unlock(&(minors[minor_number].operation_synchronizer));

	return 0;
}

static struct file_operations file_ops = {
	.owner = THIS_MODULE,
	.open = dev_open,
	.write = dev_write,
	.read = dev_read,
	.unlocked_ioctl = dev_ioctl,
	.flush = dev_flush,
	.release = dev_release
};


static int __init install_driver(void) {
	int i;

	//The array of minor struct is initializated to support the operations over device files
	for(i = 0; i < MAX_MINOR_NUMBER; i++){
		mutex_init(&(minors[i].operation_synchronizer));
		init_waitqueue_head(&(minors[i].pending_readers_wq));
		INIT_LIST_HEAD(&(minors[i].messages));
		INIT_LIST_HEAD(&(minors[i].sessions));
		INIT_LIST_HEAD(&(minors[i].pending_readings));
		minors[i].message_to_read = NULL;
		minors[i].available_readings = 0;
	}

	major_number = __register_chrdev(0, 0, MAX_MINOR_NUMBER, DEVICE_DRIVER_NAME, &file_ops);
	if (major_number < 0) {

	  AUDIT
	  printk("%s: failure in device driver registration\n", MODULE_NAME);
	  return major_number;
	}

	AUDIT
	printk("%s: success in device driver registration with major number %d\n",MODULE_NAME, major_number);

	AUDIT
	printk("%s: module successfully installed\n", MODULE_NAME);
	return 0;
}

static void __exit uninstall_driver(void){
	unregister_chrdev(major_number, DEVICE_DRIVER_NAME);

	AUDIT
	printk("%s: module successfully removed\n", MODULE_NAME);
}

module_init(install_driver);
module_exit(uninstall_driver);
