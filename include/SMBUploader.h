#ifndef SMB_UPLOADER_H
#define SMB_UPLOADER_H

#include <Arduino.h>
#include <FS.h>
#include <map>

#ifdef ENABLE_SMB_UPLOAD

// Forward declarations for libsmb2 types to avoid including headers here
struct smb2_context;
struct smb2fh;

/**
 * SMBUploader - Handles file uploads to SMB/CIFS shares
 * 
 * LIBRARY SELECTION DECISION:
 * After evaluation of available SMB libraries for ESP32, we selected libsmb2
 * (https://github.com/sahlberg/libsmb2) used as an ESP-IDF component within
 * the Arduino framework. This approach provides:
 * 
 * - Full SMB2/3 protocol support
 * - Mature, well-tested implementation
 * - Acceptable binary footprint (~220-270KB)
 * - Maintains Arduino framework compatibility via wrapper
 * 
 * Alternative esp-idf-smb-client was considered but offers no significant
 * advantage as it's also a wrapper around libsmb2 and requires ESP-IDF.
 * 
 * This class provides a clean C++ wrapper around the libsmb2 C API,
 * hiding complexity and providing Arduino-style interface.
 * 
 * Requirements: 10.1, 10.2, 10.3
 */
class SMBUploader {
private:
    String smbServer;      // Server hostname or IP
    String smbShare;       // Share name
    String smbBasePath;    // Base path within share (e.g., "test_upload_esp32")
    String smbUser;        // Username for authentication
    String smbPassword;    // Password for authentication
    
    struct smb2_context* smb2;  // libsmb2 context
    bool connected;
    
    // Pre-allocated upload buffer (avoids per-file malloc/free fragmentation).
    // When pipelining is enabled the buffer holds `pipelineDepth` contiguous
    // slots of `pipelineSlotSize` bytes each, so that up to `pipelineDepth`
    // SMB2 writes can be in flight simultaneously against the same file
    // handle. With depth = 1 the buffer is a single chunk and the upload
    // loop uses the original stop-and-wait path.
    uint8_t* uploadBuffer;
    size_t uploadBufferSize;      // total bytes allocated = depth * slotSize
    size_t pipelineSlotSize;      // bytes per pipeline slot (== per-write chunk)
    int    pipelineDepth;         // number of in-flight writes (1 = serial, >=2 = pipelined)
    
    // Upload retry override (0 = use default SMB_UPLOAD_MAX_ATTEMPTS)
    int maxUploadAttempts;

    // Cache the last verified parent directory for current SMB session to
    // avoid redundant stat/mkdir checks for every file in the same folder.
    String lastVerifiedParentDir;
    
    /**
     * Parse SMB endpoint string into server and share components
     * Expected format: //server/share or //server/share/path
     * Examples:
     *   //192.168.1.100/backups -> server=192.168.1.100, share=backups
     *   //nas.local/cpap_data -> server=nas.local, share=cpap_data
     * 
     * @param endpoint The endpoint string from config
     * @return true if parsing successful, false otherwise
     */
    bool parseEndpoint(const String& endpoint);
    
    /**
     * Establish connection to SMB share
     * Performs authentication using configured credentials
     * 
     * @return true if connection successful, false otherwise
     */
    bool connect();
    
    /**
     * Close SMB connection and cleanup resources
     */
    void disconnect();

public:
    /**
     * Constructor
     * 
     * @param endpoint SMB endpoint in format //server/share
     * @param user Username for authentication
     * @param password Password for authentication
     */
    SMBUploader(const String& endpoint, const String& user, const String& password);
    
    /**
     * Destructor - ensures cleanup
     */
    ~SMBUploader();
    
    /**
     * Initialize SMB uploader and establish connection
     * 
     * @return true if initialization successful, false otherwise
     */
    bool begin();
    
    /**
     * Create directory on SMB share (creates parent directories as needed)
     * 
     * @param path Directory path to create (e.g., "/DATALOG/20241101")
     * @return true if directory created or already exists, false on error
     */
    bool createDirectory(const String& path);
    
    /**
     * Upload a file from SD card to SMB share
     * Automatically creates parent directories if needed
     * 
     * @param localPath Path to file on SD card (e.g., "/DATALOG/20241101/file.edf")
     * @param remotePath Path on SMB share (e.g., "/DATALOG/20241101/file.edf")
     * @param sd Reference to SD card filesystem
     * @param bytesTransferred Output parameter for bytes transferred (for rate calculation)
     * @return true if upload successful, false otherwise
     */
    bool upload(const String& localPath, const String& remotePath, 
                fs::FS &sd, unsigned long& bytesTransferred);
    
    /**
     * Cleanup and disconnect
     */
    void end();
    
    /**
     * Check if currently connected to SMB share
     * 
     * @return true if connected, false otherwise
     */
    bool isConnected() const;
    
    /**
     * Pre-allocate upload buffer (must be called before first upload)
     * Should be called BEFORE Cloud TLS initialization to get clean heap.
     *
     * When `depth > 1`, allocates `depth * slotSize` bytes so the upload
     * loop can keep up to `depth` SMB2 writes in flight simultaneously
     * (SMB2 credit-based pipelining). Depth 1 preserves the original
     * stop-and-wait behaviour.
     *
     * @param slotSize Per-write chunk size (e.g., 8192, 16384, 32768)
     * @param depth Number of in-flight writes. Default 1 = no pipelining.
     *              Depth 2 doubles peak memory usage but typically ~1.7–2×
     *              throughput on a healthy LAN.
     * @return true if allocation successful, false otherwise
     */
    bool allocateBuffer(size_t slotSize, int depth = 1);
    
    /**
     * Free the upload buffer (releases heap for other phases)
     * Safe to call even if no buffer is allocated.
     */
    void freeBuffer();
    
    /**
     * Override max upload attempts per file (0 = use default).
     * Used to reduce retries outside scheduled hours.
     */
    void setMaxUploadAttempts(int attempts);

    /**
     * Server-negotiated maximum write PDU payload (bytes).
     * Valid only while connected; returns 0 otherwise.
     * Typical values: 1 MB (Samba, Windows) or 64 KB (legacy SMB servers
     * that don't advertise SMB2_GLOBAL_CAP_LARGE_MTU).
     *
     * Used by callers to right-size the upload buffer after `begin()` so
     * each `smb2_write` carries the largest payload the server will accept,
     * cutting the number of application-layer round-trips.
     */
    uint32_t getNegotiatedMaxWriteSize() const;
    
    /**
     * Scan remote directory and count files (for delta scan functionality)
     * Only counts files, not subdirectories
     * 
     * @param remotePath Path on SMB share to scan (e.g., "/DATALOG/20241101")
     * @return Number of files in the directory, -1 on error
     */
    int countRemoteFiles(const String& remotePath);
    
    /**
     * Get file information from remote directory (for deep scan functionality)
     * Returns a map of filename to file size for comparison with local files
     * 
     * @param remotePath Path on SMB share to scan (e.g., "/DATALOG/20241101")
     * @param fileInfo Output map of filename -> file size in bytes
     * @return true if successful, false on error
     */
    bool getRemoteFileInfo(const String& remotePath, std::map<String, size_t>& fileInfo);
};

#endif // ENABLE_SMB_UPLOAD

#endif // SMB_UPLOADER_H
