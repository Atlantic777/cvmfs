/**
 * This file is part of the CernVM File System.
 */

/**
 *    Backend Storage Spooler
 *    ~~~~~~~~~~~~~~~~~~~~~~~
 *
 * This is the entry point to the general file processing facility of the CVMFS
 * backend. It works with a two-stage approach:
 *
 *   1. Process file content
 *      -> create smaller file chunks for big input files
 *      -> compress the file content (optionally chunked)
 *      -> generate a content hash of the compression result
 *
 *   2. Upload files
 *      -> pluggable to support different upload pathes (local, Riak, ...)
 *
 * There are a number of different entities involved in this process. Namely:
 *   -> AbstractSpooler     - general steering tasks ( + common interface )
 *   -> FileProcessor       - chunking, compression and hashing of files
 *   -> concrete Spoolers   - upload functionality for various backend storages
 *
 * Stage 1 aka. the processing of files is handled by the FileProcessor, since
 * it is independent from the actual uploading this functionality is outsourced.
 * The FileProcessor will take care of the above mentioned steps in a concurrent
 * fashion. This process is invoked by calling AbstractSpooler::Process().
 * As a result AbstractSpooler obtains a FileProcessor::Results structure that
 * describes the processed file (chunks, checksum, compressed data location) and
 * hands it over to one of the concrete Spooler classes for upload.
 *
 * Stage 2 aka. the upload is handled by one of the concrete Spooler classes.
 * Usually the input to the upload routine is a FileProcessor::Results structure
 * which might contain several files to be uploaded (think: file chunks).
 * Depending on the implementation of the concrete Spooler we might therefore
 * produce more than one upload job for a single AbstractSpooler::Process() call.
 *
 * For some specific files we need to be able to circumvent the FileProcessor to
 * directly push them into the backend storage (i.e. .cvmfspublished), therefore
 * AbstractSpooler::Upload() is overloaded with a public method, that provides
 * this circumvention to the user. By 'user' we of course mean the classes that
 * use this Spooler facility to upload their files to the backend storage.
 *
 * In any case, calling AbstractSpooler::Process() or AbstractSpooler::Upload()
 * will invoke a callback once the whole job has been finished. Callbacks are
 * provided by the Observable template. Please see the implementation of this
 * template for more details on usage and implementation.
 * The data structure provided by this callback is called SpoolerResult and con-
 * tains information about the processed file (status, content hash, chunks, ..)
 * Note: Even if a concrete Spooler internally spawns more than one upload job
 *       to send out chunked files, the user will only see a single invocation
 *       containing information about the uploaded file including it's generated
 *       chunks.
 *
 * Workflow:
 *
 *   User
 *   \O/                Callback (SpoolerResult)
 *    |   <----------------------+
 *   / \                         |
 *    |                          |
 *    |                          |          File
 *    |  File       ################### ---------------------> #################
 *    +-----------> # AbstractSpooler #                        # FileProcessor #
 *    |             ################### <--------------------- #################
 *    |                      |    ^     FileProcessor::Results
 *    |            Hand Over |    |
 *    |                     `|´   |
 *    |  direct    #####################
 *    +----------> # Concrete Spooler  #
 *       upload    #####################
 *                           |    ^
 *                    Upload |    | Callback (SpoolerResult)
 *                          `|´   |
 *                 #####################
 *                 #  Backend Storage  #
 *                 #####################
 *
 */

#ifndef CVMFS_UPLOAD_H_
#define CVMFS_UPLOAD_H_

#include <string>
#include <cstdio>
#include <vector>

#include "hash.h"
#include "upload_file_processor.h"

namespace upload
{
  class Job;

  class BackendStat {
   public:
    BackendStat(const std::string &base_path) { base_path_ = base_path; }
    virtual ~BackendStat() { }
    virtual bool Stat(const std::string &path) = 0;
   protected:
    std::string base_path_;
  };


  class LocalStat : public BackendStat {
   public:
    LocalStat(const std::string &base_path) : BackendStat(base_path) { }
    bool Stat(const std::string &path);
  };

  BackendStat *GetBackendStat(const std::string &spooler_definition);


  // ---------------------------------------------------------------------------


  /**
   * This data structure will be passed to every callback spoolers will invoke.
   * It encapsulates the results of a spooler command along with the given
   * local_path to identify the spooler action performed.
   *
   * Note: When the return_code is different from 0 the content_hash is most
   *       likely undefined, Null or rubbish.
   */
  struct SpoolerResult {
    SpoolerResult(const int           return_code = -1,
                  const std::string  &local_path  = "",
                  const hash::Any    &digest      = hash::Any(),
                  const FileChunks   &file_chunks = FileChunks()) :
      return_code(return_code),
      local_path(local_path),
      content_hash(digest),
      file_chunks(file_chunks) {}

    inline bool IsChunked() const { return !file_chunks.empty(); }

    int         return_code;  //!< the return value of the spooler operation
    std::string local_path;   //!< the local_path previously given as input
    hash::Any   content_hash; //!< the content_hash of the bulk file derived during processing
    FileChunks  file_chunks;  //!< the file chunks generated during processing
  };


  /**
   * SpoolerDefinition is given by a string of the form:
   * <spooler type>:<spooler description>
   *
   * F.e: local:/srv/cvmfs/dev.cern.ch
   *      to define a local spooler with upstream path /srv/cvmfs/dev.cern.ch
   */
  struct SpoolerDefinition {
    enum DriverType {
      Riak,
      Local,
      Unknown
    };

    /**
     * Reads a given definition_string as described above and interprets
     * it. If the provided string turns out to be malformed the created
     * SpoolerDefinition object will not be valid. A user should check this
     * after creation using IsValid().
     *
     * @param definition_string   the spooler definition string to be inter-
     *                            preted by the constructor
     */
    SpoolerDefinition(const std::string& definition_string,
                      const bool          use_file_chunking   = false,
                      const size_t        min_file_chunk_size = 0,
                      const size_t        avg_file_chunk_size = 0,
                      const size_t        max_file_chunk_size = 0);
    bool IsValid() const { return valid_; }

    DriverType  driver_type;           //!< the type of the spooler driver
    std::string temporary_path;        //!< scratch space for the FileProcessor
    std::string spooler_configuration; //!< a driver specific spooler configuration string
                                       //!< (interpreted by the concrete spooler object)
    bool        use_file_chunking;
    size_t      min_file_chunk_size;
    size_t      avg_file_chunk_size;
    size_t      max_file_chunk_size;

    bool valid_;
  };

  /**
   * The Spooler takes care of the upload procedure of files into a backend
   * storage. It can be extended to multiple supported backend storage types,
   * like f.e. the local file system or a key value storage.
   *
   * This AbstractSpooler defines not much more than the common spooler inter-
   * face. There are derived classes that actually implement different types of
   * spoolers.
   *
   * Note: A spooler is derived from the Observable template, meaning that it
   *       allows for Listeners to be registered onto it.
   *
   * Note: Concrete implementations of AbstractSpooler are responsible to pro-
   *       duce a SpoolerResult once they finish a job and pass it upwards by
   *       invoking JobDone(). AbstractSpooler will then take care of notifying
   *       all registered listeners.
   */
  class AbstractSpooler : public Observable<SpoolerResult>,
                          public PolymorphicConstruction<AbstractSpooler,
                                                         SpoolerDefinition> {
   public:

   public:
    static void RegisterPlugins();

    virtual ~AbstractSpooler();

    /**
     * Prints the name of the concrete spooler.
     * Intended for debugging purposes only!
     */
    virtual std::string name() const = 0;

    /**
     * This method is called once before any other operations are performed on
     * a concrete Spooler. Implement this in your concrete Spooler class to do
     * global initialization work.
     *
     * TODO: In C++11 you might want to make this protected. Currently we cannot
     *       do this, since it is called by PolymorphicCreation which cannot
     *       befriend it's template parameters.
     *
     * Note: DO NOT FORGET TO UP-CALL THIS METHOD!
     */
    bool Initialize();

    /**
     * Schedules a copy job that transfers a file found at local_path to the
     * location pointed to by remote_path. Copy Jobs do not hash or compress the
     * given file. They simply upload it.
     * When the copying has finished a callback will be invoked asynchronously.
     *
     * @param local_path    path to the file which needs to be copied into the
     *                      backend storage
     * @param remote_path   the destination of the file to be copied in the
     *                      backend storage
     */
    virtual void Upload(const std::string &local_path,
                        const std::string &remote_path) = 0;

    /**
     * Schedules a process job that compresses and hashes the provided file in
     * local_path and uploads it into the CAS backend. The remote path to the
     * file is determined by the content hash of the compressed file appended by
     * file_suffix.
     * When the processing has finish a callback will be invoked asynchronously.
     *
     * Note: This method might decide to chunk the file into a number of smaller
     *       parts and upload them separately. Still, you will receive a single
     *       callback for the whole job, that contains information about the
     *       generated chunks.
     *
     * @param local_path      the location of the file to be processed and up-
     *                        loaded into the backend storage
     * @param allow_chunking  (optional) controls if this file should be cut in
     *                        chunks or uploaded at once
     */
    void Process(const std::string &local_path,
                 const bool         allow_chunking = true);

    /**
     * Blocks until all jobs currently under processing are finished. After it
     * returned, more jobs can be scheduled if needed.
     * Note: We assume that no one schedules new jobs while this method is in
     *       waiting state. Otherwise it might never return, since the job queue
     *       does not get empty.
     *
     * Note: DO NOT FORGET TO UP-CALL THIS METHOD WHEN OVERRIDING!
     */
    virtual void WaitForUpload() const;

    /**
     * Blocks until all jobs are processed and all worker threads terminated
     * successfully. Afterwards the spooler will be out of service.
     * Call this after you have called WaitForUpload() to wait until the
     * Spooler terminates.
     * Note: after calling this method NO JOBS should be scheduled anymore.
     *
     * Note: DO NOT FORGET TO UP-CALL THIS METHOD WHEN OVERRIDING!
     */
    virtual void WaitForTermination() const;

    /**
     * Checks how many of the already processed jobs have failed.
     *
     * Note: DO NOT FORGET TO UP-CALL THIS METHOD AND ADD YOUR OWN ERROR COUNT!
     *
     * @return   the number of failed jobs at the time this method is invoked
     */
    virtual unsigned int GetNumberOfErrors() const;


   protected:
    /**
     * Uploads the results of a FileProcessor job. This could be only one file
     * or a list of file chunks + one bulk version of the file.
     *
     * @param data  the results data structure obtained from the FileProcessor
     *              callback method
     */
    virtual void Upload(const FileProcessor::Results &data) = 0;

    /**
     * This method is called right before the Spooler object will terminate.
     * Implement this to do global clean up work. You should not finish jobs
     * in this method, since it is meant to be called after the Spooler has
     * stopped its actual work or was terminated prematurely.
     *
     * Note: DO NOT FORGET TO UP-CALL THIS METHOD!
     */
    virtual void TearDown();


   protected:
    /**
     * AbstractSpooler uses the PolymorphicConstruction template to generate
     * concrete spoolers. Therefore each concrete spooler must have a constructor
     * that overrides and up-calls this one.
     * Furthermore they may use the information in SpoolerDefinition
     *
     * @param spooler_definition   the SpoolerDefinition structure that defines
     *                             some intrinsics of the concrete Spoolers.
     */
    AbstractSpooler(const SpoolerDefinition &spooler_definition);

    /**
     * Concrete implementations of the AbstractSpooler must call this method
     * when they finish an upload job. A single upload job might contain more
     * than one file to be uploaded (see Upload(FileProcessor::Results) ).
     *
     * Note: If the concrete spooler implements uploading as an asynchronous
     *       task, this method MUST be called when all items for one upload
     *       job are processed.
     *
     * The concrete implementations of AbstractSpooler are responsible to fill
     * the SpoolerResult structure properly and pass it to this method.
     *
     * JobDone() will inform Listeners of the Spooler object about the finished
     * job.
     */
    void JobDone(const SpoolerResult &result);

    /**
     * Used internally: Is called when FileProcessor finishes a job.
     * Automatically takes care of processed files and prepares them for upload
     * by calling AbstractSpooler::Upload(FileProcessor::Results)
     */
    void ProcessingCallback(const FileProcessor::Results &data);

    /*
     * @return   the spooler definition that was initially given to any Spooler
     *           constructor.
     */
    inline const SpoolerDefinition& spooler_definition() const {
      return spooler_definition_;
    }

   private:
    // Status Information
    const SpoolerDefinition                      spooler_definition_;

    // File processor
    UniquePtr<ConcurrentWorkers<FileProcessor> > concurrent_processing_;
    UniquePtr<FileProcessor::worker_context >    concurrent_processing_context_;
  };

}

#endif /* CVMFS_UPLOAD_H_ */
