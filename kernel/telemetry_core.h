// --- Driver Version Selection : SELECT ONE ---

//#define BASE_CHAR_DRIVER 1 // Base character driver implementation - first version
//#define SIMPLE_CHAR_DRIVER 1 // Simple character driver implementation - second version
#define SHARED_STREAMING 1 // Shared streaming implementation - third version && current version

// --- ENDOF: Driver Version Selection :  ---

// --- Device Name Configuration ---
#define DEVICE_NAME TELEMETRY_DEVICE_NAME
// --- ENDOF: Device Name Configuration ---

// --- Major Number Configuration ---
// TODO: Consider minor number for multiple devices (e.g., telmetry_net, telmetry_disk, etc.)
// static int major_number;
// --- ENDOF: Major Number Configuration ---


// --- Ring Buffer Definition ---

#ifdef SIMPLE_CHAR_DRIVER

#define RING_BUFFER_CAPACITY 1024 
static char *kernel_char_buffer; // Buffer to store telemetry data
static int  char_read_index ____cacheline_aligned= 0; // Index to track read position
static int  char_write_index ____cacheline_aligned = 0; // Index to track write position

#else // Zero-copy driver 

#define RING_BUFFER_CAPACITY TELEMETRY_RING_BUFFER_CAPACITY
extern shared_telemetry_ring *ring_ptr;
extern wait_queue_head_t char_wait_queue;

// --- ENDOF: Ring Buffer Definition ---


#endif
