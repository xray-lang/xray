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
#include "../../base/xfileio.h"
#include "../../module/xsemver.h"
#include "../../module/xlockfile.h"
#include "../../module/xresolver.h"
#include "../../module/xpkg_client.h"
#include "../../module/xproject.h"
#include "../../base/xtoml.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

/* Build ~/.xray/packages path. Returns pointer to static buffer. */
static const char *home_packages_dir(const char *home) {
    static char buf[512];
    snprintf(buf, sizeof(buf), "%s/.xray/packages", home);
    return buf;
}

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
        xr_cli_error("pkg", "cannot get user home directory");
        return -1;
    }

    char path[512];

    // Create ~/.xray directory
    snprintf(path, sizeof(path), "%s/.xray", home);
    if (ensure_dir(path) != 0 && errno != EEXIST) {
        xr_cli_error("pkg", "cannot create directory %s", path);
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
        xr_cli_error("pkg init", "xray.toml already exists");
        return XR_CLI_EXIT_FAIL;
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
        xr_cli_error("pkg init", "cannot create xray.toml");
        return XR_CLI_EXIT_FAIL;
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
        xr_cli_error("pkg add", "missing package name");
        fprintf(stderr, "Usage: xray pkg add <owner/name>[@version]\n");
        fprintf(stderr, "Example: xray pkg add xray/redis@^1.0.0\n");
        return XR_CLI_EXIT_USAGE;
    }

    // Check if xray.toml exists
    if (!xr_cli_file_exists("xray.toml")) {
        xr_cli_error("pkg add", "xray.toml not found, run 'xray pkg init' first");
        return XR_CLI_EXIT_FAIL;
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
        xr_cli_error("pkg add", "package name must be in owner/name format");
        fprintf(stderr, "Example: xray/redis, alice/utils\n");
        return XR_CLI_EXIT_USAGE;
    }

    // Validate version constraint format
    XrVersionConstraint constraint;
    if (!xr_constraint_parse(version, &constraint)) {
        xr_cli_error("pkg add", "invalid version constraint '%s'", version);
        fprintf(stderr, "Supported formats: ^1.0.0, ~1.0.0, >=1.0.0, 1.0.0\n");
        return XR_CLI_EXIT_USAGE;
    }
    xr_constraint_free(&constraint);

    printf("Adding dependency: %s = \"%s\"\n", package, version);

    // Initialize HTTP client
    if (!xr_pkg_client_init()) {
        xr_cli_error("pkg add", "cannot initialize HTTP client");
        return XR_CLI_EXIT_FAIL;
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
        xr_cli_warn("pkg add", "cannot get package info from registry");
        fprintf(stderr, "Please add manually to xray.toml:\n");
        fprintf(stderr, "  %s = \"%s\"\n", package, version);
        xr_pkg_client_cleanup();
        return XR_CLI_EXIT_FAIL;
    }

    printf("Found package %s, %d versions\n", package, info->version_count);

    // Install package
    const char *home = getenv("HOME");
    const char *installed_ver = NULL;
    if (home && info->version_count > 0) {
        installed_ver = info->versions[0];
        if (xr_pkg_client_install(owner, name, installed_ver,
                                   home_packages_dir(home))) {
            printf("Installed %s@%s\n", package, installed_ver);
        } else {
            fprintf(stderr, "Install failed, adding to xray.toml anyway\n");
        }
    }

    xr_package_info_free(info);
    xr_pkg_client_cleanup();

    // Write dependency to xray.toml
    // Read existing content and append to [dependencies] section
    size_t toml_sz;
    char *toml = xr_file_read_all("xray.toml", "r", &toml_sz);
    if (toml) {
        FILE *f = fopen("xray.toml", "w");
        if (f) {
            // Check if [dependencies] section exists
            const char *dep_section = strstr(toml, "[dependencies]");
            if (dep_section) {
                // Find end of [dependencies] header line
                const char *after_hdr = strchr(dep_section, '\n');
                if (!after_hdr) after_hdr = dep_section + strlen(dep_section);
                else after_hdr++;
                // Write up to after [dependencies] header
                fwrite(toml, 1, (size_t)(after_hdr - toml), f);
                // Insert new dependency
                fprintf(f, "%s = \"%s\"\n", package, version);
                // Write remainder (skip if package already exists)
                const char *rest = after_hdr;
                char needle[256];
                snprintf(needle, sizeof(needle), "%s =", package);
                const char *existing = strstr(rest, needle);
                if (existing) {
                    // Replace existing line
                    fwrite(rest, 1, (size_t)(existing - rest), f);
                    // Skip old line
                    const char *eol = strchr(existing, '\n');
                    if (eol) rest = eol + 1;
                    else rest = existing + strlen(existing);
                }
                fwrite(rest, 1, strlen(rest), f);
            } else {
                // No [dependencies] section, append one
                fwrite(toml, 1, toml_sz, f);
                fprintf(f, "\n[dependencies]\n");
                fprintf(f, "%s = \"%s\"\n", package, version);
            }
            fclose(f);
            printf("Updated xray.toml\n");
        }
        xr_free(toml);
    }

    // Update lockfile
    if (installed_ver) {
        XrLockfile *lockfile = xr_lockfile_load("xray.lock");
        if (!lockfile) lockfile = xr_lockfile_new();
        if (lockfile) {
            xr_lockfile_add_package(lockfile, package, installed_ver, NULL, NULL);
            xr_lockfile_save(lockfile, "xray.lock");
            xr_lockfile_free(lockfile);
        }
    }

    return 0;
}

// xray pkg remove <package> - Remove dependency from xray.toml
static int cmd_pkg_remove(int argc, char **argv) {
    if (argc < 1) {
        xr_cli_error("pkg remove", "missing package name");
        fprintf(stderr, "Usage: xray pkg remove <owner/name>\n");
        return XR_CLI_EXIT_USAGE;
    }

    if (!xr_cli_file_exists("xray.toml")) {
        xr_cli_error("pkg remove", "xray.toml not found");
        return XR_CLI_EXIT_FAIL;
    }

    (void)argv;
    xr_cli_error("pkg remove", "not yet implemented");
    return XR_CLI_EXIT_UNAVAILABLE;
}

/*
 * Detect if an installed package is native (has native = true in xray.toml).
 * Returns true if native, false otherwise. Sets *build_system if non-NULL.
 */
static bool pkg_is_native(const char *pkg_dir) {
    char *toml_path = xr_path_join(pkg_dir, "xray.toml");
    if (!toml_path) return false;

    size_t sz;
    char *content = xr_file_read_all(toml_path, "r", &sz);
    xr_free(toml_path);
    if (!content) return false;

    XrTomlValue *root = xtoml_parse(content, sz);
    xr_free(content);
    if (!root) return false;

    XrTomlValue *pkg = xtoml_get_table(root, "package");
    if (!pkg) { xtoml_free(root); return false; }

    bool native = xtoml_get_bool_or(pkg, "native", false);
    xtoml_free(root);
    return native;
}

/*
 * Build a native package using cmake in-tree.
 * Expects CMakeLists.txt in pkg_dir.
 */
static bool pkg_build_native(const char *pkg_dir, bool verbose) {
    /* Check for CMakeLists.txt */
    char *cmake_file = xr_path_join(pkg_dir, "CMakeLists.txt");
    if (!cmake_file) return false;
    bool has_cmake = (access(cmake_file, F_OK) == 0);
    xr_free(cmake_file);

    if (!has_cmake) {
        fprintf(stderr, "Warning: native package at %s has no CMakeLists.txt\n", pkg_dir);
        return false;
    }

    /* cmake -B build -DCMAKE_BUILD_TYPE=Release */
    char build_dir[512];
    snprintf(build_dir, sizeof(build_dir), "%s/build", pkg_dir);

    if (verbose) printf("Building native package: cmake -B %s -S %s\n", build_dir, pkg_dir);

    char *cmake_argv[] = {
        "cmake", "-B", build_dir, "-S", (char*)pkg_dir,
        "-DCMAKE_BUILD_TYPE=Release", NULL
    };
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) { execvp("cmake", cmake_argv); _exit(127); }
    int status;
    if (waitpid(pid, &status, 0) < 0) return false;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "cmake configure failed\n");
        return false;
    }

    /* cmake --build build */
    char *build_argv[] = {
        "cmake", "--build", build_dir, "--parallel", "4", NULL
    };
    pid = fork();
    if (pid < 0) return false;
    if (pid == 0) { execvp("cmake", build_argv); _exit(127); }
    if (waitpid(pid, &status, 0) < 0) return false;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "cmake build failed\n");
        return false;
    }

    printf("  Native build complete\n");
    return true;
}

// xray pkg install - Install all dependencies from xray.toml
static int cmd_pkg_install(int argc, char **argv) {
    (void)argc;
    (void)argv;

    if (!xr_cli_file_exists("xray.toml")) {
        xr_cli_error("pkg install", "xray.toml not found");
        return XR_CLI_EXIT_FAIL;
    }

    if (init_global_cache() != 0) return XR_CLI_EXIT_FAIL;

    char cwd[512];
    if (!getcwd(cwd, sizeof(cwd))) {
        xr_cli_error("pkg install", "cannot get current directory");
        return XR_CLI_EXIT_FAIL;
    }

    XrProject *project = xr_project_load(NULL, cwd);
    if (!project) {
        xr_cli_error("pkg install", "cannot parse xray.toml");
        return XR_CLI_EXIT_FAIL;
    }

    if (!project->dependencies || project->dependencies->count == 0) {
        printf("No dependencies to install\n");
        xr_project_free(project);
        return 0;
    }

    printf("Installing %d dependencies...\n", project->dependencies->count);

    /* Load or create lockfile */
    XrLockfile *lockfile = xr_lockfile_load("xray.lock");
    bool had_lockfile = (lockfile != NULL);
    if (!lockfile) lockfile = xr_lockfile_new();

    if (!xr_pkg_client_init()) {
        xr_cli_warn("pkg install", "cannot initialize HTTP client");
    }

    const char *home = getenv("HOME");
    if (!home) {
        xr_cli_error("pkg install", "HOME not set");
        xr_lockfile_free(lockfile);
        xr_project_free(project);
        return XR_CLI_EXIT_FAIL;
    }

    int installed = 0, skipped = 0, failed = 0;

    /* Iterate over all dependencies */
    XrHashMap *deps = project->dependencies;
    for (uint32_t i = 0; i < deps->capacity; i++) {
        if (!deps->entries[i].key) continue;
        XrDependency *dep = (XrDependency*)deps->entries[i].value;
        if (!dep) continue;

        /* Skip local dependencies */
        if (dep->is_local) {
            printf("  %s (local: %s) — skipped\n", dep->name, dep->path);
            skipped++;
            continue;
        }

        /* Parse owner/name */
        const char *dep_name = dep->name;
        const char *slash = strchr(dep_name, '/');
        if (!slash) {
            fprintf(stderr, "  %s — invalid format (expected owner/name)\n", dep_name);
            failed++;
            continue;
        }
        char owner[128], name[128];
        size_t owner_len = (size_t)(slash - dep_name);
        if (owner_len >= sizeof(owner)) owner_len = sizeof(owner) - 1;
        memcpy(owner, dep_name, owner_len);
        owner[owner_len] = '\0';
        strncpy(name, slash + 1, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';

        /* Check if already locked */
        const XrLockedPackage *locked = xr_lockfile_find(lockfile, dep_name);
        const char *target_version = NULL;

        if (locked && locked->version) {
            target_version = locked->version;
        } else {
            /* Query registry for latest version */
            XrPackageInfo *info = xr_pkg_client_get_info(owner, name);
            if (!info || info->version_count == 0) {
                fprintf(stderr, "  %s — not found in registry\n", dep_name);
                if (info) xr_package_info_free(info);
                failed++;
                continue;
            }
            /* Pick latest version (first in list) */
            target_version = info->versions[0];

            /* Lock it */
            char resolved_url[512];
            snprintf(resolved_url, sizeof(resolved_url),
                     "%s/api/packages/%s/%s/%s/download",
                     PKG_REGISTRY_URL, owner, name, target_version);
            xr_lockfile_add_package(lockfile, dep_name, target_version,
                                    resolved_url, NULL);
            /* Use version string from info before freeing */
            target_version = xr_lockfile_find(lockfile, dep_name)->version;
            xr_package_info_free(info);
        }

        /* Check if already installed */
        char pkg_dir[512];
        snprintf(pkg_dir, sizeof(pkg_dir), "%s/.xray/packages/%s/%s/%s",
                 home, owner, name, target_version);

        char entry_check[512];
        snprintf(entry_check, sizeof(entry_check), "%s/xray.toml", pkg_dir);
        if (access(entry_check, F_OK) == 0) {
            printf("  %s@%s — already installed\n", dep_name, target_version);
            skipped++;
            continue;
        }

        /* Download + extract */
        printf("  %s@%s — downloading...\n", dep_name, target_version);
        if (!xr_pkg_client_install(owner, name, target_version,
                                    home_packages_dir(home))) {
            fprintf(stderr, "  %s@%s — install failed\n", dep_name, target_version);
            failed++;
            continue;
        }

        /* Detect native and build */
        if (pkg_is_native(pkg_dir)) {
            printf("  %s@%s — native package, building...\n", dep_name, target_version);
            if (!pkg_build_native(pkg_dir, false)) {
                fprintf(stderr, "  %s@%s — native build failed\n", dep_name, target_version);
                failed++;
                continue;
            }
        }

        installed++;
    }

    /* Save lockfile */
    if (lockfile->package_count > 0) {
        xr_lockfile_save(lockfile, "xray.lock");
        if (!had_lockfile) printf("\nGenerated xray.lock\n");
    }

    printf("\nDone: %d installed, %d skipped, %d failed\n",
           installed, skipped, failed);

    xr_lockfile_free(lockfile);
    xr_project_free(project);
    xr_pkg_client_cleanup();

    return failed > 0 ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_OK;
}

// xray pkg update [package] - Update dependencies
static int cmd_pkg_update(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Check if xray.toml exists
    if (!xr_cli_file_exists("xray.toml")) {
        xr_cli_error("pkg update", "xray.toml not found");
        return XR_CLI_EXIT_FAIL;
    }

    xr_cli_error("pkg update", "not yet implemented");
    return XR_CLI_EXIT_UNAVAILABLE;
}

// xray pkg tree - Show dependency tree
static int cmd_pkg_tree(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Check if xray.toml exists
    if (!xr_cli_file_exists("xray.toml")) {
        xr_cli_error("pkg tree", "xray.toml not found");
        return XR_CLI_EXIT_FAIL;
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
        xr_cli_error("pkg login", "login failed");
        return XR_CLI_EXIT_FAIL;
    }

    // Save token
    if (!xr_pkg_client_save_token(token)) {
        xr_cli_warn("pkg login", "cannot save login credentials");
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
        xr_cli_error("pkg publish", "xray.toml not found");
        return XR_CLI_EXIT_FAIL;
    }

    // Load project config
    char cwd[512];
    if (!getcwd(cwd, sizeof(cwd))) {
        xr_cli_error("pkg publish", "cannot get current directory");
        return XR_CLI_EXIT_FAIL;
    }

    XrProject *project = xr_project_load(NULL, cwd);
    if (!project) {
        xr_cli_error("pkg publish", "cannot parse xray.toml");
        return XR_CLI_EXIT_FAIL;
    }

    // Check if it's a publishable package
    if (!project->is_package) {
        xr_cli_error("pkg publish", "not a publishable package");
        fprintf(stderr, "Use [package] instead of [project] in xray.toml\n");
        xr_project_free(project);
        return XR_CLI_EXIT_FAIL;
    }

    // Check version
    if (!project->version || !xr_semver_is_valid(project->version)) {
        xr_cli_error("pkg publish", "missing valid version number");
        fprintf(stderr, "Add to xray.toml: version = \"1.0.0\"\n");
        xr_project_free(project);
        return XR_CLI_EXIT_FAIL;
    }

    printf("Publishing %s@%s...\n", project->name, project->version);

    // Load auth token
    char *token = NULL;
    if (!xr_pkg_client_load_token(&token)) {
        xr_cli_error("pkg publish", "run 'xray pkg login' first");
        xr_project_free(project);
        return XR_CLI_EXIT_FAIL;
    }

    // Create package tarball
    char tarball[512];
    snprintf(tarball, sizeof(tarball), "/tmp/%s-%s.tar.gz",
             project->name, project->version);

    printf("Packaging...\n");
    if (!create_tarball(tarball)) {
        xr_cli_error("pkg publish", "packaging failed");
        xr_free(token);
        xr_project_free(project);
        return XR_CLI_EXIT_FAIL;
    }

    // Calculate checksum
    char checksum[72];
    if (xr_lockfile_checksum_file(tarball, checksum)) {
        printf("Checksum: %s\n", checksum);
    }

    // Publish
    if (!xr_pkg_client_init()) {
        xr_cli_error("pkg publish", "cannot initialize HTTP client");
        xr_free(token);
        xr_project_free(project);
        return XR_CLI_EXIT_FAIL;
    }

    XrPkgPublishInfo pub_info = {
        .name = project->name,
        .version = project->version,
        .description = project->description,
        .license = project->license,
    };
    bool success = xr_pkg_client_publish(tarball, token, &pub_info);

    // Cleanup
    unlink(tarball);
    xr_free(token);
    xr_project_free(project);
    xr_pkg_client_cleanup();

    return success ? XR_CLI_EXIT_OK : XR_CLI_EXIT_FAIL;
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
