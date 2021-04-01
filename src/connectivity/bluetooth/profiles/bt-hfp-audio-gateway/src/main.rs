// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#![recursion_limit = "1024"]

use {
    anyhow::{Context, Error},
    fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    futures::{channel::mpsc, future, pin_mut},
    log::warn,
};

use crate::{
    config::AudioGatewayFeatureSupport, fidl_service::run_services, hfp::Hfp, profile::Profile,
};

mod config;
mod error;
mod fidl_service;
mod hfp;
mod peer;
mod procedure;
mod profile;
mod protocol;
mod service_definitions;

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    fuchsia_syslog::init().context("Could not initialize logger")?;

    let feature_support = AudioGatewayFeatureSupport::load()?;
    let profile = Profile::register_audio_gateway(feature_support)?;

    let (call_manager_sender, call_manager_receiver) = mpsc::channel(1);

    let hfp = Hfp::new(profile, call_manager_receiver, feature_support).run();
    pin_mut!(hfp);

    let mut fs = ServiceFs::new();

    let inspector = fuchsia_inspect::Inspector::new();
    if let Err(e) = inspector.serve(&mut fs) {
        warn!("Could not serve inspect: {}", e);
    }

    let services = run_services(fs, call_manager_sender);
    pin_mut!(services);

    match future::select(services, hfp).await {
        future::Either::Left((Ok(()), _)) => {
            log::warn!("Service FS directory handle closed. Exiting.");
        }
        future::Either::Left((Err(e), _)) => {
            log::error!("Error encountered running Service FS: {}. Exiting", e);
        }
        future::Either::Right((Ok(()), _)) => {
            log::warn!(
                "All Hfp related connections to this component have been disconnected. Exiting."
            );
        }
        future::Either::Right((Err(e), _)) => {
            log::error!("Error encountered running main Hfp loop: {}. Exiting.", e);
        }
    }

    Ok(())
}
