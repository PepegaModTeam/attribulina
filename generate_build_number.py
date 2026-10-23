import subprocess

def get_version_components(version_tag):
    try:
        # Extract major, minor, and revision components from the version tag
        version_components = version_tag.strip('v').split('.')
        major, minor, revision = map(int, version_components)

        return major, minor, revision
    except ValueError:
        return 0, 0, 0

def get_git_info():
    try:
        # Get the commit count, commit ID, latest tag, and branch name from the local repository
        commit_count_result = subprocess.run(["git", "rev-list", "--count", "HEAD"], stdout=subprocess.PIPE, text=True, check=True)
        commit_id_result = subprocess.run(["git", "rev-parse", "--short", "HEAD"], stdout=subprocess.PIPE, text=True, check=True)
        latest_tag_result = subprocess.run(["git", "describe", "--abbrev=0", "--tags"], stdout=subprocess.PIPE, text=True, check=True)
        branch_name_result = subprocess.run(["git", "rev-parse", "--abbrev-ref", "HEAD"], stdout=subprocess.PIPE, text=True, check=True)

        commit_count = commit_count_result.stdout.strip()
        commit_id = commit_id_result.stdout.strip()
        version_tag = latest_tag_result.stdout.strip()
        branch_name = branch_name_result.stdout.strip()

        major, minor, revision = get_version_components(version_tag)

        return commit_count, commit_id, major, minor, revision, branch_name
    except subprocess.CalledProcessError:
        return "0", "unknown", 0, 0, 0, "unknown"

if __name__ == "__main__":
    build_number, commit_id, major, minor, revision, branch_name = get_git_info()

    with open("include/build_number.h", "w") as file:
        file.write(f"#define BUILD_NUMBER {build_number}\n")
        file.write(f"#define COMMIT_ID \"{commit_id}\"\n")
        file.write(f"#define VERSION_MAJOR {major}\n")
        file.write(f"#define VERSION_MINOR {minor}\n")
        file.write(f"#define VERSION_REVISION {revision}\n")
        file.write(f"#define BRANCH_NAME \"{branch_name}\"\n")