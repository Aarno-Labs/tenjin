#![feature(rustc_private)]

fn main() {
    env_logger::init();
    rustc_plugin::cli_main(xj_improve_multitool::XjImproveMultitoolPlugin);
}
