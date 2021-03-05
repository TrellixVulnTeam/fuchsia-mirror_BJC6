// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Error,
    argh::FromArgs,
    carnelian::{
        color::Color,
        facet::{FacetId, Scene, SceneBuilder, SetLocationMessage, ShedFacet},
        input, make_app_assistant,
        render::Context,
        App, AppAssistant, Point, Rect, RenderOptions, Size, ViewAssistant, ViewAssistantContext,
        ViewAssistantPtr, ViewKey,
    },
    euclid::{default::Point2D, size2},
    fuchsia_trace_provider,
    fuchsia_zircon::Event,
    std::{collections::BTreeMap, path::PathBuf},
};

const BACKGROUND_COLOR: Color = Color { r: 255, g: 255, b: 255, a: 255 };
const SPACING_FRACTION: f32 = 0.8;

/// Svg.
#[derive(Debug, FromArgs)]
#[argh(name = "svg-rs")]
struct Args {
    /// use spinel (GPU rendering back-end)
    #[argh(switch, short = 's')]
    use_spinel: bool,
}

#[derive(Default)]
struct SvgAppAssistant {
    use_spinel: bool,
}

impl AppAssistant for SvgAppAssistant {
    fn setup(&mut self) -> Result<(), Error> {
        let args: Args = argh::from_env();
        self.use_spinel = args.use_spinel;
        Ok(())
    }

    fn create_view_assistant(&mut self, _: ViewKey) -> Result<ViewAssistantPtr, Error> {
        Ok(Box::new(SvgViewAssistant::new()))
    }

    fn get_render_options(&self) -> RenderOptions {
        RenderOptions { use_spinel: self.use_spinel }
    }
}

struct SceneDetails {
    scene: Scene,
    facet_id: FacetId,
}

struct SvgViewAssistant {
    scene_details: Option<SceneDetails>,
    touch_locations: BTreeMap<input::pointer::PointerId, Point2D<f32>>,
    position: Point,
}

impl SvgViewAssistant {
    pub fn new() -> Self {
        Self { scene_details: None, touch_locations: BTreeMap::new(), position: Point::zero() }
    }
}

impl ViewAssistant for SvgViewAssistant {
    fn setup(&mut self, _context: &ViewAssistantContext) -> Result<(), Error> {
        Ok(())
    }

    fn resize(&mut self, _new_size: &Size) -> Result<(), Error> {
        self.scene_details = None;
        Ok(())
    }

    fn render(
        &mut self,
        render_context: &mut Context,
        ready_event: Event,
        context: &ViewAssistantContext,
    ) -> Result<(), Error> {
        let mut scene_details = self.scene_details.take().unwrap_or_else(|| {
            let location = Rect::from_size(context.size).center();
            self.position = location;
            let mut builder = SceneBuilder::new(BACKGROUND_COLOR);
            let edge_size = context.size.width.min(context.size.height) * SPACING_FRACTION;
            let shed_facet = ShedFacet::new(
                PathBuf::from("/pkg/data/static/fuchsia.shed"),
                location,
                size2(edge_size, edge_size),
            );
            let shed_facet_id = builder.facet(Box::new(shed_facet));
            let scene = builder.build();
            SceneDetails { scene, facet_id: shed_facet_id }
        });

        scene_details.scene.render(render_context, ready_event, context)?;
        self.scene_details = Some(scene_details);

        Ok(())
    }

    fn handle_pointer_event(
        &mut self,
        context: &mut ViewAssistantContext,
        _event: &input::Event,
        pointer_event: &input::pointer::Event,
    ) -> Result<(), Error> {
        match &pointer_event.phase {
            input::pointer::Phase::Down(touch_location) => {
                self.touch_locations
                    .insert(pointer_event.pointer_id.clone(), touch_location.to_f32());
            }
            input::pointer::Phase::Moved(touch_location) => {
                if let Some(location) = self.touch_locations.get_mut(&pointer_event.pointer_id) {
                    // Pan image using the change to touch location.
                    self.position += touch_location.to_f32() - *location;
                    *location = touch_location.to_f32();
                }
                if let Some(scene_details) = self.scene_details.as_mut() {
                    scene_details.scene.send_message(
                        &scene_details.facet_id,
                        Box::new(SetLocationMessage { location: self.position }),
                    );
                }
            }

            input::pointer::Phase::Up => {
                self.touch_locations.remove(&pointer_event.pointer_id.clone());
            }
            _ => (),
        }
        context.request_render();
        Ok(())
    }
}

fn main() -> Result<(), Error> {
    fuchsia_trace_provider::trace_provider_create_with_fdio();

    println!("Svg Example");
    App::run(make_app_assistant::<SvgAppAssistant>())
}
