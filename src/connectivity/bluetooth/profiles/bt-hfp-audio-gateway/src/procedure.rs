// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {at_commands as at, std::fmt, thiserror::Error};

use crate::{
    indicator_status::IndicatorStatus,
    peer::service_level_connection::{Command, SlcState},
    protocol::features::AgFeatures,
};

/// Defines the implementation of the Call Waiting Notifications Procedure.
pub mod call_waiting_notifications;

/// Defines the implementation of the Report Extended Audio Gateway Error Code Results Procedure.
pub mod extended_errors;

/// Defines the implementation of the SLC Initialization Procedure.
pub mod slc_initialization;

/// Defines the implementation of the NR/EC Procedure.
pub mod nrec;

/// Defines the implementation of the Query Operator Selection Procedure.
pub mod query_operator_selection;

use call_waiting_notifications::CallWaitingNotificationsProcedure;
use extended_errors::ExtendedErrorsProcedure;
use nrec::NrecProcedure;
use query_operator_selection::QueryOperatorProcedure;
use slc_initialization::SlcInitProcedure;

/// Errors that can occur during the operation of an HFP Procedure.
#[derive(Clone, Error, Debug)]
pub enum ProcedureError {
    #[error("Unexpected AG procedural update: {:?}", .0)]
    UnexpectedAg(at::Response),
    #[error("Unparseabled HF procedural update: {:?}", .0)]
    UnparsableHf(at::DeserializeError),
    #[error("Unexpected HF procedural update: {:?}", .0)]
    UnexpectedHf(at::Command),
    #[error("Unexpected procedure request")]
    UnexpectedRequest,
    #[error("Procedure has already terminated")]
    AlreadyTerminated,
    #[error("Procedure not implemented")]
    NotImplemented,
    #[error("Error in the service level connection: {:?}", .0)]
    Channel(fuchsia_zircon::Status),
}

impl From<fuchsia_zircon::Status> for ProcedureError {
    fn from(src: fuchsia_zircon::Status) -> Self {
        ProcedureError::Channel(src)
    }
}

impl From<&Command> for ProcedureError {
    fn from(src: &Command) -> Self {
        match src {
            Command::Ag(cmd) => Self::UnexpectedAg(cmd.clone()),
            Command::Hf(cmd) => Self::UnexpectedHf(cmd.clone()),
        }
    }
}

/// A unique identifier associated with an HFP procedure.
// TODO(fxbug.dev/70591): Add to this enum as more procedures are implemented.
#[derive(Copy, Clone, Debug, Hash, Eq, PartialEq)]
pub enum ProcedureMarker {
    /// The Service Level Connection Initialization procedure as defined in HFP v1.8 Section 4.2.
    SlcInitialization,
    /// The Noise Reduction/Echo Cancellation procedure as defined in HFP v1.8 Section 4.24.
    Nrec,
    /// The Query Operator Selection procedure as defined in HFP v1.8 Section 4.8.
    QueryOperatorSelection,
    /// The Extended Audio Gateway Error Results Code as defined in HFP v1.8 Section 4.9.
    ExtendedErrors,
    /// The Extended Audio Gateway Error Results Code as defined in HFP v1.8 Section 4.21.
    CallWaitingNotifications,
}

impl ProcedureMarker {
    /// Initializes a new procedure for the current marker.
    pub fn initialize(&self) -> Box<dyn Procedure> {
        match self {
            Self::SlcInitialization => Box::new(SlcInitProcedure::new()),
            Self::Nrec => Box::new(NrecProcedure::new()),
            Self::QueryOperatorSelection => Box::new(QueryOperatorProcedure::new()),
            Self::ExtendedErrors => Box::new(ExtendedErrorsProcedure::new()),
            Self::CallWaitingNotifications => Box::new(CallWaitingNotificationsProcedure::new()),
        }
    }

    /// Matches the HF `command` to a procedure. Returns an error if the command is
    /// unable to be matched.
    fn match_hf_command(command: &at::Command) -> Result<Self, ProcedureError> {
        match command {
            at::Command::Brsf { .. }
            | at::Command::Bac { .. }
            | at::Command::CindTest { .. }
            | at::Command::CindRead { .. }
            | at::Command::Cmer { .. }
            | at::Command::ChldTest { .. }
            | at::Command::BindTest { .. }
            | at::Command::BindRead { .. } => Ok(Self::SlcInitialization),
            at::Command::Nrec { .. } => Ok(Self::Nrec),
            at::Command::Cops { .. } | at::Command::CopsRead { .. } => {
                Ok(Self::QueryOperatorSelection)
            }
            at::Command::Cmee { .. } => Ok(Self::ExtendedErrors),
            at::Command::Ccwa { .. } => Ok(Self::CallWaitingNotifications),
            _ => Err(ProcedureError::NotImplemented),
        }
    }

    /// Matches the AG `command` to a procedure. Returns an error if the command is
    /// unable to be matched.
    fn match_ag_command(_command: &at::Response) -> Result<Self, ProcedureError> {
        Err(ProcedureError::NotImplemented)
    }

    pub fn match_command(command: &Command) -> Result<Self, ProcedureError> {
        match command {
            Command::Ag(cmd) => Self::match_ag_command(cmd),
            Command::Hf(cmd) => Self::match_hf_command(cmd),
        }
    }
}

/// The requests generated by an HFP procedure as it progresses through its state machine.
pub enum ProcedureRequest {
    /// AT messages to be sent to the peer (HF) - requires no response.
    SendMessages(Vec<at::Response>),

    /// Information requests - use the `response` fn to build a response to the request.
    // TODO(fxbug.dev/70591): Add to this list once more procedures are implemented.
    GetAgFeatures {
        response: Box<dyn FnOnce(AgFeatures) -> at::Response>,
    },
    GetAgIndicatorStatus {
        response: Box<dyn FnOnce(IndicatorStatus) -> at::Response>,
    },
    GetNetworkOperatorName {
        response: Box<dyn FnOnce(Option<String>) -> at::Response>,
    },

    SetNrec {
        enable: bool,
        response: Box<dyn FnOnce(Result<(), ()>) -> at::Response>,
    },

    /// Error from processing an update.
    Error(ProcedureError),

    /// No-op.
    None,
}

impl ProcedureRequest {
    pub fn is_err(&self) -> bool {
        match &self {
            Self::Error(_) => true,
            _ => false,
        }
    }

    /// Returns true if this request requires a response.
    pub fn requires_response(&self) -> bool {
        match &self {
            Self::GetAgFeatures { .. }
            | Self::GetAgIndicatorStatus { .. }
            | Self::GetNetworkOperatorName { .. }
            | Self::SetNrec { .. } => true,
            _ => false,
        }
    }

    pub fn send_one_message_and_ok(message: at::Response) -> Self {
        vec![message, at::Response::Ok].into()
    }
}

impl From<at::Response> for ProcedureRequest {
    fn from(message: at::Response) -> Self {
        vec![message].into()
    }
}

impl From<Vec<at::Response>> for ProcedureRequest {
    fn from(messages: Vec<at::Response>) -> Self {
        Self::SendMessages(messages)
    }
}

impl fmt::Debug for ProcedureRequest {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let other;
        let output = match &self {
            Self::GetAgFeatures { .. } => "GetAgFeatures",
            Self::GetAgIndicatorStatus { .. } => "GetAgIndicatorStatus",
            Self::SetNrec { enable: true, .. } => "SetNrec(enabled)",
            Self::SetNrec { enable: false, .. } => "SetNrec(disabled)",
            Self::GetNetworkOperatorName { .. } => "GetNetworkOperatorName",
            event => {
                other = format!("{:?}", event);
                &other
            }
        }
        .to_string();
        write!(f, "{}", output)
    }
}

/// An interface to interact with an HFP Procedure.
pub trait Procedure {
    /// Returns the unique identifier associated with this procedure.
    fn marker(&self) -> ProcedureMarker;

    /// Receive an HF `update` to progress the procedure. Returns a request
    /// to the update.
    ///
    /// `update` is the incoming AT message received from the HF.
    /// `state` is the shared state associated with the service level connection and may be
    /// modified when applying the update.
    ///
    /// There are no guarantees if `hf_update()` is called on a Procedure that is terminated
    /// (namely, `is_terminated()` returns true) and may result in an error request.
    /// The handling of unexpected or invalid updates is procedure dependent.
    ///
    /// Developers should ensure that the final request of a Procedure does not require
    /// a response.
    fn hf_update(&mut self, update: at::Command, _state: &mut SlcState) -> ProcedureRequest {
        ProcedureRequest::Error(ProcedureError::UnexpectedHf(update))
    }

    /// Receive an AG `update` to progress the procedure. Returns a request
    /// to the update.
    ///
    /// `update` is the incoming AT message received from the AG.
    /// `state` is the shared state associated with the service level connection and may be
    /// modified when applying the update.
    ///
    /// There are no guarantees if `ag_update()` is called on a Procedure that is terminated
    /// (namely, `is_terminated()` returns true) and may result in an error request.
    /// The handling of unexpected or invalid updates is procedure dependent.
    ///
    /// Developers should ensure that the final request of a Procedure does not require
    /// a response.
    fn ag_update(&mut self, update: at::Response, _state: &mut SlcState) -> ProcedureRequest {
        ProcedureRequest::Error(ProcedureError::UnexpectedAg(update))
    }

    /// Returns true if the Procedure is finished.
    fn is_terminated(&self) -> bool {
        false
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use matches::assert_matches;

    /// A single response converts to the expected request
    #[test]
    fn at_response_to_procedure_request_conversion() {
        let message = at::Response::Ok;
        let request: ProcedureRequest = message.into();
        assert_matches!(
            request,
            ProcedureRequest::SendMessages(messages) if messages == vec![at::Response::Ok]
        );
    }

    /// A vec of responses converts to the expected request
    #[test]
    fn at_responses_to_procedure_request_conversion() {
        let messages = vec![at::Response::Ok, at::Response::Error];
        let request: ProcedureRequest = messages.into();
        assert_matches!(
            request,
            ProcedureRequest::SendMessages(messages)
                if messages == vec![at::Response::Ok, at::Response::Error]
        );
    }
}
