#include <linux/ioctl.h>

//Ioctl commands
#define SET_SEND_TIMEOUT _IO('a', 0)
#define SET_RECV_TIMEOUT _IO('a', 1)
#define REVOKE_DELAYED_MESSAGES _IO('a', 2)

#ifdef __KERNEL__

#define MODULE_NAME "TIMED-MESSAGING-SYSTEM"
#define DEVICE_DRIVER_NAME "timed-messaging-system"
#define MAX_MINOR_NUMBER 8
#define AUDIT if(1)
#define DEFAULT_MAX_MESSAGE_SIZE 64
#define DEFAULT_MAX_STORAGE_SIZE 1280
#define DEFAULT_SEND_TIMEOUT 0
#define DEFAULT_RECV_TIMEOUT 0

//minor struct collect the metadata needed to manage a device file with a specified minor number
struct minor {
	wait_queue_head_t pending_readers_wq; 	//used during blocked readings
	struct mutex operation_synchronizer;	//to synchronize the operation on device file
	struct list_head messages; 				//list of messages posted on device file
	struct list_head sessions; 				//list of open sessions on device file
	struct list_head *message_to_read; 		//pointer to next message to read
	struct list_head pending_readings; 		//list of pending readings on device file
	size_t storage_size; 					//bytes used by device file to store messages
	int available_readings;					//number of available readings on device file
};

//session struct collect the metadata needed to manage session open on a device file
struct session {
	struct list_head list;					
	struct workqueue_struct *workqueue;		//used during delayed writes
	struct list_head pending_writes;		//list of pending writes of the session
	struct mutex session_mutex;				//to synchronize the operation on the session
	long send_timeout; 						//timeout before a read returns 
	long recv_timeout;						//timeout before a write message is posted
};

//message struct represents a message in the system
struct message {
	struct list_head list;					
	bool is_delayed;						//true if the posting of message is delayed
	size_t size;							//the size in bytes of the message	
	char *text;								//the content of the message
};

//pending_write represents a delayed write in the system
struct pending_write{
	struct list_head list;	
	struct delayed_work delayed_work;		//the work to do when timer expires
	struct message message;					//the message to post
	int minor_number;						//minor number of device target for writing
};

//pending_read represents a blocked read in the system
struct pending_read {
	struct list_head list;
	bool is_flushed;						//true if anyone call flush() on the device file
};


/* The open function creates a session to the file specified by pathname and adds this to the list of sessions associated to device file.
the just created sesssion has send_timeout and recv_timeout set to zero by default. The pointer to that session is stored in the 
private_data field of struct file. The open returns 0 in case of success.*/
static int dev_open(struct inode *inode, struct file *file);

/* The release function closes the session to the file specified by file descriptor and remove this from the list of session associated
to device file. Before complete the close the workqueue containing the delayed writes started on this session is waited; then the workqueue is destroyed. The release returns 0 in case of success.*/
static int dev_release(struct inode *inode, struct file *file);

/* The write function allows to post a message on the message queue of the device file specified througth the struct file passed in input.
Others params are buff and len, respectively the message to write and its size. The offset off is unused.
When a write occours first the size of message is checked not be over the maximum size allowed and is checked also the total storage space, of the device file the write occours on, not be over the maximum size allowed. If these checks fail the write is aborted, otherwise can occours.
So the message is created, if this can be immediatly posted (send_timeout is zero) it is linked to the list of message of the device file and it is ready to be read. Otherwise, a pending_write struct containg a delayed work consisting in the write is created and linked to the list of pending write associated to session the write occours on. The message previously created is copied into this struct and the original one is freed. The write returns the number of written chars, 0 in case of delayed write, -1 in case of error */
static ssize_t dev_write(struct file *file, const char *buff, size_t len, loff_t *off);

/* The open function allows to read a message from the message queue of the device file specified througth the struct file passed in input.
Others params are buff and len, respectively the buffer where the caller wants receive the message and its size. The offset off is unused.
The read is always not blocking if there are messages to read. If there are not the read is blocking only in the case recv_timeout is not zero. In this case a pending_read struct is created and linked to the others in a list associated to the device file. The thread asking for a blocking read start to sleep on the waitqueue of the device file until a new message is posted or flush operation is invoked. If a new message is posted the read occours, if flush operation is invoked the read is aborted. The read returns the number of read chars, -1 in case of absence of message to read */
static ssize_t dev_read(struct file *file, char *buff, size_t len, loff_t *off);

/* The ioctl function allows to manage the session to a device file specified by the file input parameter. The other parama are the command to execute and the param for this command. The available commands are SET_SEND_TIMEOUT that sets the send_timeout to the value specified by param, SET_RECT_TIMEOUT that sets the recv_timeout to the value specified by param and REVOKE_DELAYED_MESSAGE that revokes the post of all delayed message on the current session. The ioctl returns 0 in case of success.*/
static long dev_ioctl(struct file *file, unsigned int command, unsigned long param);

/* The flush operation allows to unblock all the blocked readers on the device file and cancel all delayed writes across all sessions open for device file. To unblock the blocked readers the function iterates over the list of pending read associated to device file and set to true the flag is_flushed. Then wakes up all the thread sleeping on the waitqueue. When schedulated these threads will be check the flush condition and will abort the read operation. The flush returns 0 in case of success */
static int dev_flush(struct file *file, fl_owner_t id);

#endif
