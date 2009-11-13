#ifndef READ_PARALLEL_HPP
#define READ_PARALLEL_HPP

#include "MBForward.hpp"
#include "MBReaderIface.hpp"

#include <string>

class MBReadUtilIface;
class MBParallelComm;

class ReadParallel
{
   
public:

  static MBReaderIface* factory( MBInterface* );

    //! load a file
  MBErrorCode load_file(const char *file_name,
                        const MBEntityHandle* file_set,
                        const FileOptions &opts,
                        const MBReaderIface::IDTag* subset_list = 0,
                        int subset_list_length = 0,
                        const MBTag* file_id_tag = 0 );
  
    //! load multiple files
  MBErrorCode load_file(const char **file_names,
                        const int num_files,
                        const MBEntityHandle* file_set,
                        const FileOptions &opts,
                        const MBReaderIface::IDTag* subset_list = 0,
                        int subset_list_length = 0,
                        const MBTag* file_id_tag = 0 );
  
  MBErrorCode load_file(const char **file_names,
                        const int num_files,
                        const MBEntityHandle* file_set,
                        int parallel_mode, 
                        std::string &partition_tag_name, 
                        std::vector<int> &partition_tag_vals, 
                        bool distrib,
                        bool partition_by_rank,
                        std::vector<int> &pa_vec,
                        const FileOptions &opts,
                        const MBReaderIface::IDTag* subset_list,
                        int subset_list_length,
                        const MBTag* file_id_tag,
                        const int reader_rank,
                        const bool cputime,
                        const int resolve_dim,
                        const int shared_dim,
                        const int ghost_dim,
                        const int bridge_dim,
                        const int num_layers);
    //! Constructor
  ReadParallel(MBInterface* impl = NULL, MBParallelComm *pc = NULL);

   //! Destructor
  virtual ~ReadParallel() {}

  static const char *parallelOptsNames[];
  
  enum ParallelActions {PA_READ=0, 
                        PA_READ_PART, 
                        PA_BROADCAST, 
                        PA_DELETE_NONLOCAL,
                        PA_CHECK_GIDS_SERIAL, 
                        PA_GET_FILESET_ENTS, 
                        PA_RESOLVE_SHARED_ENTS,
                        PA_EXCHANGE_GHOSTS, 
                        PA_PRINT_PARALLEL};

  static const char *ParallelActionsNames[];
  
  enum ParallelOpts { POPT_NONE=0, 
                      POPT_BCAST, 
                      POPT_BCAST_DELETE, 
                      POPT_READ_DELETE, 
                      POPT_READ_PART, 
                      POPT_DEFAULT};

    //! PUBLIC TO ALLOW TESTING
  MBErrorCode delete_nonlocal_entities(std::string &ptag_name,
                                       std::vector<int> &ptag_vals,
                                       bool distribute,
                                       MBEntityHandle file_set);
  
  MBErrorCode delete_nonlocal_entities(MBEntityHandle file_set);

protected:
  MBErrorCode create_partition_sets( std::string &ptag_name,
                                     MBEntityHandle file_set );

private:

  MBInterface *mbImpl;

    // each reader can keep track of its own pcomm
  MBParallelComm *myPcomm;
};

inline MBErrorCode ReadParallel::load_file(const char *file_name,
                                           const MBEntityHandle* file_set,
                                           const FileOptions &opts,
                                           const MBReaderIface::IDTag* subset_list,
                                           int subset_list_length,
                                           const MBTag* file_id_tag )
{
  return load_file(&file_name, 1, file_set, opts, 
                   subset_list, subset_list_length, file_id_tag);
}
  
#endif
