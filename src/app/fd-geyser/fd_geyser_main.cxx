#include "local-pragmas.h"

#include "fd_geyser_service.hxx"
#include "fd_compact_encoder.h"

#include <iostream>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <regex>
#include <csignal>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/initialize.h"
#include "absl/strings/str_format.h"

extern "C" {
/* Use C++ safe headers */
#include "fd_geyser_cxx_api.h"
#include "../../util/fd_util_base.h"
#include "../../util/log/fd_log.h"

/* Declarations from shmem - avoid including full header */
extern char  fd_shmem_private_base[ 256 ];
extern ulong fd_shmem_private_base_len;

void fd_boot( int * pargc, char *** pargv );
void fd_halt( void );
}

ABSL_FLAG(std::string, config, "", "Path to firedancer config TOML file (auto-derives workspace names and exec_tile_cnt)");
ABSL_FLAG(std::string, mount_path, "/mnt/.fd", "Mount path for shared memory workspaces");
ABSL_FLAG(uint16_t, port, 8754, "Server port for the gRPC service");
ABSL_FLAG(std::string, funk_wksp, "", "Funk workspace name (auto-derived from config if not specified)");
ABSL_FLAG(std::string, replay_out_wksp, "", "Replay output workspace (auto-derived from config if not specified)");
ABSL_FLAG(std::string, tower_out_wksp, "", "Tower output workspace (auto-derived from config if not specified)");
ABSL_FLAG(uint32_t, exec_tile_cnt, 0, "Number of exec tiles (auto-derived from config if not specified)");
ABSL_FLAG(std::string, exec_geyser_wksp_prefix, "", "Prefix for exec_geyser workspaces (auto-derived from config if not specified)");
ABSL_FLAG(bool, debug, false, "Enable DEBUG level logging to logfile");

/* Signal handler for dynamic log level switching
   SIGUSR1: Enable DEBUG logging
   SIGUSR2: Disable DEBUG logging (back to NOTICE) */
static void log_level_signal_handler(int sig) {
  if (sig == SIGUSR1) {
    fd_log_level_logfile_set(0);  /* 0 = DEBUG */
    FD_LOG_WARNING(("Received SIGUSR1: DEBUG logging ENABLED"));
  } else if (sig == SIGUSR2) {
    fd_log_level_logfile_set(2);  /* 2 = NOTICE */
    FD_LOG_WARNING(("Received SIGUSR2: DEBUG logging DISABLED"));
  }
}

/* Signal handler for compact encoding toggle
   SIGHUP: Toggle compact encoding on/off */
static void compact_toggle_signal_handler(int sig) {
  (void)sig;
  fd_compact_toggle();
  FD_LOG_WARNING(("Received SIGHUP: Compact encoding %s",
                  fd_compact_is_enabled() ? "ENABLED" : "DISABLED"));
}

/* Simple TOML config parser - extracts only the fields we need */
struct FdGeyserConfig {
  std::string name;           /* From top-level name = "..." */
  uint32_t exec_tile_count;   /* From [layout] exec_tile_count = N */
  std::string hugetlbfs_mount_path;  /* From [hugetlbfs] mount_path = "..." */

  FdGeyserConfig() : exec_tile_count(0) {}
};

static bool parse_config_file(const std::string& path, FdGeyserConfig& cfg) {
  std::ifstream file(path);
  if (!file.is_open()) {
    std::cerr << "Failed to open config file: " << path << std::endl;
    return false;
  }

  std::string line;
  bool in_layout_section = false;
  bool in_hugetlbfs_section = false;

  /* Regex patterns - use standard escaping instead of raw strings */
  std::regex name_re("^\\s*name\\s*=\\s*\"([^\"]+)\"");
  std::regex exec_tile_re("^\\s*exec_tile_count\\s*=\\s*(\\d+)");
  std::regex mount_path_re("^\\s*mount_path\\s*=\\s*\"([^\"]+)\"");
  std::regex section_re("^\\s*\\[([^\\]]+)\\]");

  while (std::getline(file, line)) {
    std::smatch match;

    /* Check for section headers */
    if (std::regex_search(line, match, section_re)) {
      std::string section = match[1].str();
      in_layout_section = (section == "layout");
      in_hugetlbfs_section = (section == "hugetlbfs");
      continue;
    }

    /* Parse top-level name (before any section) */
    if (!in_layout_section && !in_hugetlbfs_section && cfg.name.empty()) {
      if (std::regex_search(line, match, name_re)) {
        cfg.name = match[1].str();
        continue;
      }
    }

    /* Parse exec_tile_count in [layout] section */
    if (in_layout_section) {
      if (std::regex_search(line, match, exec_tile_re)) {
        cfg.exec_tile_count = std::stoul(match[1].str());
        continue;
      }
    }

    /* Parse mount_path in [hugetlbfs] section */
    if (in_hugetlbfs_section) {
      if (std::regex_search(line, match, mount_path_re)) {
        cfg.hugetlbfs_mount_path = match[1].str();
        continue;
      }
    }
  }

  if (cfg.name.empty()) {
    std::cerr << "Config file missing 'name' field" << std::endl;
    return false;
  }
  if (cfg.exec_tile_count == 0) {
    std::cerr << "Config file missing 'layout.exec_tile_count' field" << std::endl;
    return false;
  }

  return true;
}

void
RunServer(uint16_t port, fd_geyser_ctx_t * ctx) {
  std::string server_address = absl::StrFormat("0.0.0.0:%d", port);
  GeyserServiceImpl service( ctx );

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  grpc::ServerBuilder builder;

  /* Listen on the given address without any authentication */
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());

  /* Set resource quota */
  grpc::ResourceQuota quota;
  quota.SetMaxThreads(4);
  builder.SetResourceQuota(quota);

  /* Set max message sizes */
  builder.SetMaxReceiveMessageSize(64 * 1024 * 1024);  /* 64 MB */
  builder.SetMaxSendMessageSize(64 * 1024 * 1024);     /* 64 MB */

  /* Register the service */
  builder.RegisterService(&service);

  /* Build and start the server */
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  std::cout << "fd-geyser server listening on " << server_address << std::endl;

  /* Wait for shutdown */
  server->Wait();
}

int main(int argc, char** argv) {
  fd_boot( &argc, &argv );

  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();

  /* Set stderr to WARNING level (only WARNING and above go to stdout/stderr)
     Log levels: 0=DEBUG, 1=INFO, 2=NOTICE, 3=WARNING, 4=ERR */
  fd_log_level_stderr_set( 3 ); /* 3 = WARNING */

  /* Set logfile level based on --debug flag
     Log levels: 0=DEBUG, 1=INFO, 2=NOTICE, 3=WARNING, 4=ERR */
  if( absl::GetFlag(FLAGS_debug) ) {
    fd_log_level_logfile_set( 0 ); /* 0 = DEBUG */
    FD_LOG_WARNING(( "DEBUG logging enabled (logfile level=DEBUG, stderr level=WARNING)" ));
  } else {
    fd_log_level_logfile_set( 2 ); /* 2 = NOTICE */
    FD_LOG_WARNING(( "Normal logging (logfile level=NOTICE, stderr level=WARNING)" ));
  }

  /* Register signal handlers for dynamic log level switching */
  std::signal(SIGUSR1, log_level_signal_handler);
  std::signal(SIGUSR2, log_level_signal_handler);
  std::signal(SIGHUP, compact_toggle_signal_handler);
  FD_LOG_NOTICE(( "Signal handlers registered:" ));
  FD_LOG_NOTICE(( "  kill -USR1 <pid>  Enable DEBUG logging" ));
  FD_LOG_NOTICE(( "  kill -USR2 <pid>  Disable DEBUG logging" ));
  FD_LOG_NOTICE(( "  kill -HUP <pid>   Toggle compact encoding" ));

  /* Parse config file if provided */
  FdGeyserConfig file_cfg;
  std::string config_path = absl::GetFlag(FLAGS_config);
  bool has_config = !config_path.empty();

  if (has_config) {
    if (!parse_config_file(config_path, file_cfg)) {
      FD_LOG_ERR(( "Failed to parse config file: %s", config_path.c_str() ));
      return 1;
    }
    FD_LOG_NOTICE(( "Loaded config: name=%s, exec_tile_count=%u, mount_path=%s",
                    file_cfg.name.c_str(),
                    file_cfg.exec_tile_count,
                    file_cfg.hugetlbfs_mount_path.empty() ? "(default)" : file_cfg.hugetlbfs_mount_path.c_str() ));
  }

  /* Derive values from config file or use command line flags */
  std::string mount_path = absl::GetFlag(FLAGS_mount_path);
  if (has_config && !file_cfg.hugetlbfs_mount_path.empty()) {
    mount_path = file_cfg.hugetlbfs_mount_path;
  }

  std::string funk_wksp = absl::GetFlag(FLAGS_funk_wksp);
  if (funk_wksp.empty() && has_config) {
    funk_wksp = file_cfg.name + "_funk.wksp";
  }

  std::string replay_out_wksp = absl::GetFlag(FLAGS_replay_out_wksp);
  if (replay_out_wksp.empty() && has_config) {
    replay_out_wksp = file_cfg.name + "_replay_out.wksp";
  }

  std::string tower_out_wksp = absl::GetFlag(FLAGS_tower_out_wksp);
  if (tower_out_wksp.empty() && has_config) {
    tower_out_wksp = file_cfg.name + "_tower_out.wksp";
  }

  uint32_t exec_tile_cnt = absl::GetFlag(FLAGS_exec_tile_cnt);
  if (exec_tile_cnt == 0 && has_config) {
    exec_tile_cnt = file_cfg.exec_tile_count;
  }

  std::string exec_prefix = absl::GetFlag(FLAGS_exec_geyser_wksp_prefix);
  if (exec_prefix.empty() && has_config) {
    exec_prefix = file_cfg.name + "_geyser";  /* Workspace pattern: fd1_geyser_0, fd1_geyser_1, ... */
  }

  /* Validate required values */
  if (funk_wksp.empty() || replay_out_wksp.empty() || tower_out_wksp.empty() || exec_tile_cnt == 0 || exec_prefix.empty()) {
    FD_LOG_ERR(( "Missing required parameters. Either provide --config or specify all workspace names and exec_tile_cnt." ));
    return 1;
  }

  /* Set up shared memory mount path */
  strncpy( fd_shmem_private_base, mount_path.c_str(), sizeof(fd_shmem_private_base)-1 );
  fd_shmem_private_base_len = mount_path.length();

  FD_LOG_NOTICE(( "Using mount_path: %s", mount_path.c_str() ));
  FD_LOG_NOTICE(( "Using funk_wksp: %s", funk_wksp.c_str() ));
  FD_LOG_NOTICE(( "Using replay_out_wksp: %s", replay_out_wksp.c_str() ));
  FD_LOG_NOTICE(( "Using tower_out_wksp: %s", tower_out_wksp.c_str() ));
  FD_LOG_NOTICE(( "Using exec_tile_cnt: %u", exec_tile_cnt ));
  FD_LOG_NOTICE(( "Using exec_geyser_wksp_prefix: %s", exec_prefix.c_str() ));

  /* Build geyser arguments */
  fd_geyser_args_t args;
  memset( &args, 0, sizeof(args) );

  strncpy( args.funk_wksp, funk_wksp.c_str(), 31 );
  strncpy( args.replay_out_wksp, replay_out_wksp.c_str(), 31 );
  strncpy( args.tower_out_wksp, tower_out_wksp.c_str(), 31 );

  args.exec_tile_cnt = exec_tile_cnt;

  for( ulong i = 0; i < args.exec_tile_cnt; i++ ) {
    snprintf( args.exec_geyser_wksp[i], 31, "%s_%lu.wksp", exec_prefix.c_str(), i );
    FD_LOG_NOTICE(( "exec_geyser workspace %lu: %s", i, args.exec_geyser_wksp[i] ));
  }

  /* Initialize the geyser context */
  fd_geyser_ctx_t * ctx = fd_geyser_init( &args );

  /* Start the polling loop in a background thread */
  std::thread poll_thread([ctx](){
    fd_geyser_loop( ctx );
  });

  /* Run the gRPC server (blocks) */
  RunServer( absl::GetFlag(FLAGS_port), ctx );

  poll_thread.join();

  fd_halt();
  return 0;
}
