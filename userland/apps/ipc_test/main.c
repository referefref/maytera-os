// ipc_test - Test application for IPC syscalls (Shared Memory + Message Passing)
// This tests Tasks #31 and #32

#include "../../libc/maytera.h"
#include "../../libc/ipc.h"

void test_shared_memory(void) {
    printf("\n========================================\n");
    printf("[IPC_TEST] Testing Shared Memory...\n");
    printf("========================================\n");

    // Create shared memory region (4KB)
    printf("[SHM] Creating 4KB shared memory region...\n");
    int shm_id = shm_create(4096, SHM_FLAG_NONE);
    if (shm_id < 0) {
        printf("[SHM] ERROR: Failed to create shared memory!\n");
        return;
    }
    printf("[SHM] SUCCESS: Created region with ID %d\n", shm_id);

    // Map the shared memory
    void *addr = NULL;
    printf("[SHM] Mapping shared memory...\n");
    if (shm_map(shm_id, &addr) != 0) {
        printf("[SHM] ERROR: Failed to map shared memory!\n");
        shm_destroy(shm_id);
        return;
    }
    printf("[SHM] SUCCESS: Mapped at address %p\n", addr);

    // Write to shared memory
    printf("[SHM] Writing test data to shared memory...\n");
    char *data = (char *)addr;
    const char *test_string = "Hello from IPC test!";
    for (int i = 0; test_string[i]; i++) {
        data[i] = test_string[i];
    }
    data[20] = '\0';
    printf("[SHM] Wrote: '%s'\n", data);

    // Read back
    printf("[SHM] Reading back: '%s'\n", data);

    // Get info
    size_t size = 0;
    unsigned int ref_count = 0;
    if (shm_info(shm_id, &size, &ref_count) == 0) {
        printf("[SHM] Info: size=%lu, ref_count=%u\n", (unsigned long)size, ref_count);
    }

    // Unmap
    printf("[SHM] Unmapping shared memory...\n");
    if (shm_unmap(shm_id) != 0) {
        printf("[SHM] ERROR: Failed to unmap!\n");
    } else {
        printf("[SHM] SUCCESS: Unmapped\n");
    }

    // Destroy
    printf("[SHM] Destroying shared memory region...\n");
    if (shm_destroy(shm_id) != 0) {
        printf("[SHM] ERROR: Failed to destroy!\n");
    } else {
        printf("[SHM] SUCCESS: Destroyed\n");
    }

    printf("[SHM] Shared memory test complete!\n");
}

void test_message_passing(void) {
    printf("\n========================================\n");
    printf("[IPC_TEST] Testing Message Passing...\n");
    printf("========================================\n");

    // Create a channel
    printf("[MSG] Creating message channel...\n");
    int channel_id = msg_create_channel();
    if (channel_id < 0) {
        printf("[MSG] ERROR: Failed to create channel!\n");
        return;
    }
    printf("[MSG] SUCCESS: Created channel with ID %d\n", channel_id);

    // Accept to get server connection
    printf("[MSG] Getting server connection...\n");
    int server_conn = msg_accept(channel_id, 0);
    if (server_conn < 0) {
        printf("[MSG] ERROR: Failed to get server connection!\n");
        msg_destroy_channel(channel_id);
        return;
    }
    printf("[MSG] SUCCESS: Server connection ID %d\n", server_conn);

    // Connect as a client (same process for testing)
    printf("[MSG] Connecting as client...\n");
    int client_conn = msg_connect(channel_id);
    if (client_conn < 0) {
        printf("[MSG] ERROR: Failed to connect as client!\n");
        msg_destroy_channel(channel_id);
        return;
    }
    printf("[MSG] SUCCESS: Client connection ID %d\n", client_conn);

    // Send a message from client to server
    const char *msg = "Hello from client!";
    printf("[MSG] Client sending: '%s'\n", msg);
    long sent = msg_send(client_conn, msg, 19);
    if (sent < 0) {
        printf("[MSG] ERROR: Failed to send message!\n");
    } else {
        printf("[MSG] SUCCESS: Sent %ld bytes\n", sent);
    }

    // Server receives message
    char buf[64] = {0};
    printf("[MSG] Server receiving...\n");
    long received = msg_recv(server_conn, buf, sizeof(buf), 100);
    if (received > 0) {
        printf("[MSG] SUCCESS: Received %ld bytes: '%s'\n", received, buf);
    } else if (received == 0) {
        printf("[MSG] Timeout - no message received\n");
    } else {
        printf("[MSG] ERROR: recv failed!\n");
    }

    // Get channel info
    unsigned int num_conns = 0;
    if (msg_channel_info(channel_id, &num_conns) == 0) {
        printf("[MSG] Channel info: %u connections\n", num_conns);
    }

    // Close client connection
    printf("[MSG] Closing client connection...\n");
    if (msg_close(client_conn) == 0) {
        printf("[MSG] SUCCESS: Client connection closed\n");
    }

    // Destroy channel
    printf("[MSG] Destroying channel...\n");
    if (msg_destroy_channel(channel_id) == 0) {
        printf("[MSG] SUCCESS: Channel destroyed\n");
    }

    printf("[MSG] Message passing test complete!\n");
}

int main(void) {
    printf("========================================\n");
    printf("[IPC_TEST] MayteraOS IPC Test Suite\n");
    printf("========================================\n");
    printf("[IPC_TEST] Testing Tasks #31 (Shared Memory)\n");
    printf("[IPC_TEST]    and Task #32 (Message Passing)\n");

    // Test shared memory
    test_shared_memory();

    // Test message passing
    test_message_passing();

    printf("\n========================================\n");
    printf("[IPC_TEST] All tests complete!\n");
    printf("========================================\n");

    return 0;
}
