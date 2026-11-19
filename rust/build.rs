fn main() {
    println!("cargo:rerun-if-changed=resources/app.ico");
    println!("cargo:rerun-if-changed=resources/app.rc");
    embed_resource::compile(
        "resources/app.rc",
        embed_resource::ParamsIncludeDirs(["resources"]),
    )
    .manifest_required()
    .unwrap();
}
