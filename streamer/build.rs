// Embed Info.plist into the executable. macOS only grants System Audio
// Recording (and shows its prompt) to a process that declares why it wants
// it; a bare command-line binary has nowhere else to say so.
fn main() {
    let plist = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("Info.plist");
    println!("cargo:rerun-if-changed={}", plist.display());
    println!("cargo:rustc-link-arg=-Wl,-sectcreate,__TEXT,__info_plist,{}", plist.display());
}
