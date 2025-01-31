// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{self, Error},
    tracing,
};

#[fuchsia::test]
async fn my_component_test_rust_test() -> Result<(), Error> {
    // Connect to the component(s) under test using the Realm protocol, e.g.
    // This assumes that the child component exposes the `fuchsia.component.Binder`
    // protocol. For more information on this protocol, see: 
    // https://fuchsia.dev/fuchsia-src/concepts/components/v2/component_manifests#framework-protocols.
    // If your component exposes another capability, you connect to it directly.
    // ```
    // use fuchsia_component::client as fclient;
    // use fidl_fuchsia_component as fcomponent;
    // use fidl_fuchsia_sys2 as fsys;
    // use fidl_fuchsia_io as fio;
    // use fidl::endpoints;
    //
    // let realm_proxy = fclient::realm()?;
    // let (exposed_directory, server_end) = endpoints::create_proxy::<fio::DirectoryMarker>()?;
    // let () = realm_proxy
    //     .open_exposed_dir(
    //         &mut fsys::ChildRef { name: "hello-world".to_string(), collection: None },
    //         server_end,
    //     )
    //     .await?;
    // let _: fcomponent::BinderProxy = fclient::connect_to_protocol_at_dir_root::<fcomponent::BinderMarker>(&exposed_directory)?;
    // ```


    // Use the ArchiveReader to access inspect data, e.g.
    // ```
    // use diagnostics_reader::{ArchiveReader, Inspect};
    //
    // let reader = ArchiveReader::new().add_selector("hello-world:root");
    // let results = reader.snapshot::<Inspect>().await?;
    // ```


    // Add test conditions here, e.g.
    // ```
    // let expected_string = test_function();
    // ```

    tracing::debug!("Initialized.");

    // Assert conditions here, e.g.
    // ```
    // assert_eq!(expected_string, "Hello World!");
    // ```

    Ok(())
}
