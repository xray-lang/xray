/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_pkg.c - 'xray pkg' package management CLI commands
 *
 * KEY CONCEPT:
 *   Implements package management commands:
 *   - xray pkg init      Initialize a new project
 *   - xray pkg add       Add a dependency
 *   - xray pkg remove    Remove a dependency
 *   - xray pkg install   Install all dependencies
 *   - xray pkg update    Update dependencies
 *   - xray pkg tree      Display dependency tree
 *   - xray pkg login     Login to package registry
 *   - xray pkg publish   Publish a package
 */

#include "xcli.h"
#include "xcli_spec.h"
#include "xcli_fs.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../module/xsemver.h"
#include "../../module/xlockfile.h"
#include "../../module/xresolver.h"
#include "../../module/xpkg_client.h"
#include "../../module/xproject.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

// Create directory if not exists
static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    return mkdir(path, 0755);
}


// Initialize global cache directory structure
static int init_global_cache(void) {
    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "Error: cannot get user home directory\n");
        return -1;
    }

    char path[512];

    // Create ~/.xray directory
    snprintf(path, sizeof(path), "%s/.xray", home);
    if (ensure_dir(path) != 0 && errno != EEXIST) {
        fprintf(stderr, "Error: cannot create directory %s\n", path);
        return -1;
    }

    // Create ~/.xray/cache directory
    snprintf(path, sizeof(path), "%s/.xray/cache", home);
    ensure_dir(path);

    // Create ~/.xray/packages directory
    snprintf(path, sizeof(path), "%s/.xray/packages", home);
    ensure_dir(path);

    // Create ~/.xray/bin directory
    snprintf(path, sizeof(path), "%s/.xray/bin", home);
    ensure_dir(path);

    return 0;
}

/*
 * Create tarball safely using fork/exec instead of system().
 * Avoids shell injection vulnerabilities.
 */
static bool create_tarball(const char *output_path) {
    pid_t pid = fork();
    if (pid < 0) return false;

    if (pid == 0) {
        // Child process
        execlp("tar", "tar", "-czf", output_path,
               "--exclude=.git", "--exclude=node_modules", ".", NULL);
        _exit(127);
    }

    int status;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// xray pkg init - Initialize project, create xray.toml
static int cmd_pkg_init(int argc, char **argv) {
    const char *name = NULL;

    // Parse arguments
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        }
    }

    // Check if xray.toml already exists
    if (xr_cli_file_exists("xray.toml")) {
        fprintf(stderr, "Error: xray.toml already exists\n");
        return 1;
    }

    // If name not specified, use current directory name
    char default_name[256] = "my-project";
    if (!name) {
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd))) {
            char *last_slash = strrchr(cwd, '/');
            if (last_slash) {
                strncpy(default_name, last_slash + 1, sizeof(default_name) - 1);
            }
        }
        name = default_name;
    }

    // Create xray.toml
    FILE *f = fopen("xray.toml", "w");
    if (!f) {
        fprintf(stderr, "Error: cannot create xray.toml\n");
        return 1;
    }

    fprintf(f, "# xray.toml - Project configuration\n\n");
    fprintf(f, "[project]\n");
    fprintf(f, "name = \"%s\"\n", name);
    fprintf(f, "main = \"src/main.xr\"\n");
    fprintf(f, "\n");
    fprintf(f, "[dependencies]\n");
    fprintf(f, "# Example:\n");
    fprintf(f, "# xray/redis = \"^1.0.0\"\n");
    fprintf(f, "# alice/utils = \"^1.0.0\"\n");

    fclose(f);

    // Create src directory
    ensure_dir("src");

    // Create example main.xr
    if (!xr_cli_file_exists("src/main.xr")) {
        f = fopen("src/main.xr", "w");
        if (f) {
            fprintf(f, "// %s - Main entry\n\n", name);
            fprintf(f, "print(\"Hello, Xray!\")\n");
            fclose(f);
        }
    }

    printf("Project initialized\n");
    printf("  - xray.toml created\n");
    printf("  - src/main.xr created\n");
    printf("\n");
    printf("Next steps:\n");
    printf("  xray src/main.xr       Run project\n");
    printf("  xray pkg add <package> Add dependency\n");

    return 0;
}

// xray pkg add <package>[@version] - Add dependency to xray.toml and install
static int cmd_pkg_add(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "Usage: xray pkg add <owner/name>[@version]\n");
        fprintf(stderr, "Example: xray pkg add xray/redis\n");
        fprintf(stderr, "         xray pkg add xray/redis@^1.0.0\n");
        return 1;
    }

    // Check if xray.toml exists
    if (!xr_cli_file_exists("xray.toml")) {
        fprintf(stderr, "Error: xray.toml not found, run 'xray pkg init' first\n");
        return 1;
    }

    // Parse package name and version
    char package_buf[256];
    strncpy(package_buf, argv[0], sizeof(package_buf) - 1);
    package_buf[sizeof(package_buf) - 1] = '\0';

    char *package = package_buf;
    char *version = "^1.0.0";  // Default version
    char *at_sign = strchr(package, '@');
    if (at_sign) {
        *at_sign = '\0';
        version = at_sign + 1;
    }

    // Validate package name format (owner/name)
    char *slash = strchr(package, '/');
    if (!slash) {
        fprintf(stderr, "Error: package name must be in owner/name format\n");
        fprintf(stderr, "Example: xray/redis, alice/utils\n");
        return 1;
    }

    // Validate version constraint format
    XrVersionConstraint constraint;
    if (!xr_constraint_parse(version, &constraint)) {
        fprintf(stderr, "Error: invalid version constraint '%s'\n", version);
        fprintf(stderr, "Supported formats: ^1.0.0, ~1.0.0, >=1.0.0, 1.0.0\n");
        return 1;
    }
    xr_constraint_free(&constraint);

    printf("Adding dependency: %s = \"%s\"\n", package, version);

    // Initialize HTTP client
    if (!xr_pkg_client_init()) {
        fprintf(stderr, "Error: cannot initialize HTTP client\n");
        return 1;
    }

    // Parse owner and name
    char owner[128], name[128];
    *slash = '\0';
    strncpy(owner, package, sizeof(owner) - 1);
    strncpy(name, slash + 1, sizeof(name) - 1);
    *slash = '/';  // Restore

    // Try to get package info from registry
    XrPackageInfo *info = xr_pkg_client_get_info(owner, name);
    if (!info) {
        printf("Warning: cannot get package info from registry\n");
        printf("Please add manually to xray.toml:\n");
        printf("  %s = \"%s\"\n", package, version);
        xr_pkg_client_cleanup();
        return 0;
    }

    printf("Found package %s, %d versions\n", package, info->version_count);

    // Install package
    const char *home = getenv("HOME");
    if (home) {
        char dest_dir[512];
        snprintf(dest_dir, sizeof(dest_dir), "%s/.xray/packages", home);

        // Select best version
        if (info->version_count > 0) {
            const char *best_ver = info->versions[0];  // Simplified: take first
            if (xr_pkg_client_install(owner, name, best_ver, dest_dir)) {
                printf("Installed %s@%s\n", package, best_ver);
            }
        }
    }

    xr_package_info_free(info);
    xr_pkg_client_cleanup();

    printf("\nPlease add to xray.toml:\n");
    printf("  %s = \"%s\"\n", package, version);

    return 0;
}

// xray pkg remove <package> - Remove dependency from xray.toml
static int cmd_pkg_remove(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "Usage: xray pkg remove <owner/name>\n");
        return 1;
    }

    // Check if xray.toml exists
    if (!xr_cli_file_exists("xray.toml")) {
        fprintf(stderr, "Error: xray.toml not found\n");
        return 1;
    }

    const char *package = argv[0];
    printf("Removing dependency: %s\n", package);

    fprintf(stderr, "Error: 'pkg remove' is not yet implemented\n");

    return 0;
}

// xray pkg install - Install all dependencies from xray.toml
static int cmd_pkg_install(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Check if xray.toml exists
    if (!xr_cli_file_exists("xray.toml")) {
        fprintf(stderr, "Error: xray.toml not found\n");
        return 1;
    }

    // Initialize global cache
    if (init_global_cache() != 0) {
        return 1;
    }

    printf("Installing dependencies...\n");

    // Load project config
    char cwd[512];
    if (!getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "Error: cannot get current directory\n");
        return 1;
    }

    XrProject *project = xr_project_load(NULL, cwd);
    if (!project) {
        fprintf(stderr, "Error: cannot parse xray.toml\n");
        return 1;
    }

    // Check if there are dependencies
    if (!project->dependencies || project->dependencies->count == 0) {
        printf("No dependencies to install\n");
        xr_project_free(project);
        return 0;
    }

    printf("Found %d dependencies\n", project->dependencies->count);

    // Try to load existing lockfile
    XrLockfile *lockfile = xr_lockfile_load("xray.lock");
    if (lockfile) {
        printf("Using locked versions from xray.lock\n");
    } else {
        lockfile = xr_lockfile_new();
    }

    // Initialize HTTP client
    if (!xr_pkg_client_init()) {
        fprintf(stderr, "Warning: cannot initialize HTTP client\n");
    }

    // Create dependency graph
    XrDepGraph *graph = xr_depgraph_new();

    // Traverse dependencies and add to graph
    // Simplified, actual implementation needs HashMap traversal
    printf("Dependency resolver ready, waiting for registry deployment\n");

    // Save lockfile
    if (lockfile->package_count > 0) {
        xr_lockfile_save(lockfile, "xray.lock");
        printf("Generated xray.lock\n");
    }

    // Cleanup
    xr_depgraph_free(graph);
    xr_lockfile_free(lockfile);
    xr_project_free(project);
    xr_pkg_client_cleanup();

    return 0;
}

// xray pkg update [package] - Update dependencies
static int cmd_pkg_update(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Check if xray.toml exists
    if (!xr_cli_file_exists("xray.toml")) {
        fprintf(stderr, "Error: xray.toml not found\n");
        return 1;
    }

    printf("Updating dependencies...\n");

    fprintf(stderr, "Error: 'pkg update' is not yet implemented\n");

    return 0;
}

// xray pkg tree - Show dependency tree
static int cmd_pkg_tree(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Check if xray.toml exists
    if (!xr_cli_file_exists("xray.toml")) {
        fprintf(stderr, "Error: xray.toml not found\n");
        return 1;
    }

    // Try to load lockfile
    XrLockfile *lockfile = xr_lockfile_load("xray.lock");
    if (lockfile && lockfile->package_count > 0) {
        printf("Dependency tree (from xray.lock):\n\n");

        for (int i = 0; i < lockfile->package_count; i++) {
            const XrLockedPackage *pkg = &lockfile->packages[i];
            printf("├── %s@%s\n", pkg->name, pkg->version ? pkg->version : "*");

            for (int j = 0; j < pkg->dep_count; j++) {
                const char *is_last = (j == pkg->dep_count - 1) ? "└" : "├";
                printf("│   %s── %s\n", is_last, pkg->dependencies[j]);
            }
        }

        xr_lockfile_free(lockfile);
    } else {
        printf("xray.lock not found, run 'xray pkg install' first\n");
    }

    return 0;
}

// xray pkg login - Login to package registry
static int cmd_pkg_login(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("Logging in to pkg.xray-lang.org...\n\n");

    // Check if already logged in
    char *existing_token = NULL;
    if (xr_pkg_client_load_token(&existing_token)) {
        printf("Login credentials already exist.\n");
        printf("Re-login? (y/N) ");
        fflush(stdout);

        char answer[16];
        if (fgets(answer, sizeof(answer), stdin)) {
            if (answer[0] != 'y' && answer[0] != 'Y') {
                printf("Cancelled\n");
                xr_free(existing_token);
                return 0;
            }
        }
        xr_free(existing_token);
    }

    // Execute login flow
    char *token = NULL;
    if (!xr_pkg_client_login(&token)) {
        fprintf(stderr, "Login failed\n");
        return 1;
    }

    // Save token
    if (!xr_pkg_client_save_token(token)) {
        fprintf(stderr, "Warning: cannot save login credentials\n");
    }

    xr_free(token);
    printf("\nLogin successful\n");

    return 0;
}

// xray pkg publish - Publish package
static int cmd_pkg_publish(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Check if xray.toml exists
    if (!xr_cli_file_exists("xray.toml")) {
        fprintf(stderr, "Error: xray.toml not found\n");
        return 1;
    }

    // Load project config
    char cwd[512];
    if (!getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "Error: cannot get current directory\n");
        return 1;
    }

    XrProject *project = xr_project_load(NULL, cwd);
    if (!project) {
        fprintf(stderr, "Error: cannot parse xray.toml\n");
        return 1;
    }

    // Check if it's a publishable package
    if (!project->is_package) {
        fprintf(stderr, "Error: this is not a publishable package\n");
        fprintf(stderr, "Use [package] instead of [project] in xray.toml\n");
        xr_project_free(project);
        return 1;
    }

    // Check version
    if (!project->version || !xr_semver_is_valid(project->version)) {
        fprintf(stderr, "Error: missing valid version number\n");
        fprintf(stderr, "Add to xray.toml: version = \"1.0.0\"\n");
        xr_project_free(project);
        return 1;
    }

    printf("Publishing %s@%s...\n", project->name, project->version);

    // Load auth token
    char *token = NULL;
    if (!xr_pkg_client_load_token(&token)) {
        fprintf(stderr, "Error: run 'xray pkg login' first\n");
        xr_project_free(project);
        return 1;
    }

    // Create package tarball
    char tarball[512];
    snprintf(tarball, sizeof(tarball), "/tmp/%s-%s.tar.gz",
             project->name, project->version);

    printf("Packaging...\n");
    if (!create_tarball(tarball)) {
        fprintf(stderr, "Error: packaging failed\n");
        xr_free(token);
        xr_project_free(project);
        return 1;
    }

    // Calculate checksum
    char checksum[72];
    if (xr_lockfile_checksum_file(tarball, checksum)) {
        printf("Checksum: %s\n", checksum);
    }

    // Publish
    if (!xr_pkg_client_init()) {
        fprintf(stderr, "Error: cannot initialize HTTP client\n");
        xr_free(token);
        xr_project_free(project);
        return 1;
    }

    bool success = xr_pkg_client_publish(tarball, token);

    // Cleanup
    unlink(tarball);
    xr_free(token);
    xr_project_free(project);
    xr_pkg_client_cleanup();

    return success ? 0 : 1;
}

/* Subcommand entry — unified parser has already consumed --help.
 * Positionals from inv carry the subcommand name and its arguments. */
XR_FUNC int cmd_pkg(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");

    if (inv->positional_count < 1) {
        /* No subcommand — dispatch already shows subcommand help */
        xr_cli_error("pkg", "no subcommand specified");
        return XR_CLI_EXIT_USAGE;
    }

    const char *subcmd = inv->positionals[0];
    /* Internal subcommand argc/argv (skip the subcommand name itself) */
    int sub_argc = inv->positional_count - 1;
    char **sub_argv = (char **)inv->positionals + 1;

    if (strcmp(subcmd, "init") == 0)
        return cmd_pkg_init(sub_argc, sub_argv);
    if (strcmp(subcmd, "add") == 0)
        return cmd_pkg_add(sub_argc, sub_argv);
    if (strcmp(subcmd, "remove") == 0)
        return cmd_pkg_remove(sub_argc, sub_argv);
    if (strcmp(subcmd, "install") == 0)
        return cmd_pkg_install(sub_argc, sub_argv);
    if (strcmp(subcmd, "update") == 0)
        return cmd_pkg_update(sub_argc, sub_argv);
    if (strcmp(subcmd, "tree") == 0)
        return cmd_pkg_tree(sub_argc, sub_argv);
    if (strcmp(subcmd, "login") == 0)
        return cmd_pkg_login(sub_argc, sub_argv);
    if (strcmp(subcmd, "publish") == 0)
        return cmd_pkg_publish(sub_argc, sub_argv);

    xr_cli_error("pkg", "unknown subcommand '%s'", subcmd);
    return XR_CLI_EXIT_USAGE;
}
