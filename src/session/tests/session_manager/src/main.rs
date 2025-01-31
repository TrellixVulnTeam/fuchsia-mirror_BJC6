// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[cfg(test)]
mod tests {
    use {
        fidl_fuchsia_sys2 as fsys, fuchsia_component::client::connect_to_protocol,
        session_manager_lib,
    };

    const GRAPHICAL_SESSION_URL: &'static str = "#meta/graphical-session.cm";

    /// Passes if the root session launches successfully. This tells us:
    ///     - session_manager is able to use the Realm service to launch a component.
    ///     - the root session was started in the "session" collection.
    ///     - capability routing of the Scenic service to the session collection was successful.
    #[fuchsia_async::run_singlethreaded(test)]
    async fn launch_root_session() {
        let realm = connect_to_protocol::<fsys::RealmMarker>().expect("could not connect to Realm");

        let session_url = String::from(GRAPHICAL_SESSION_URL);
        println!("Session url: {}", &session_url);

        session_manager_lib::startup::launch_session(&session_url, &realm)
            .await
            .expect("Failed to run session");
    }
}
