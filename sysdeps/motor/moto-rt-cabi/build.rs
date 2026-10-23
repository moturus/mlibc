use std::path::PathBuf;

fn parse_version(contents: &str, prefix: &str, suffix: char) -> u64 {
    let value = contents
        .lines()
        .find_map(|line| line.trim().strip_prefix(prefix))
        .unwrap_or_else(|| panic!("missing version declaration starting with {prefix:?}"));
    value
        .trim()
        .strip_suffix(suffix)
        .unwrap_or_else(|| panic!("version declaration {value:?} has no {suffix:?} suffix"))
        .parse()
        .unwrap_or_else(|_| panic!("version declaration {value:?} is not an integer"))
}

fn main() {
    let manifest_dir = PathBuf::from(std::env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let header = manifest_dir.join("moto_rt.h");
    println!("cargo:rerun-if-changed={}", header.display());

    let header_source = std::fs::read_to_string(&header).unwrap();
    let header_version = parse_version(&header_source, "#define MOTO_RT_VERSION ", 'u');
    let output = PathBuf::from(std::env::var_os("OUT_DIR").unwrap()).join("rt_version.rs");
    // Check the actual target dependency, without locating its source tree.
    std::fs::write(
        output,
        format!(
            "const _: () = assert!(moto_rt::RT_VERSION == {header_version}, \
             \"MOTO_RT_VERSION in moto_rt.h must match moto_rt::RT_VERSION\");\n"
        ),
    )
    .unwrap();
}
