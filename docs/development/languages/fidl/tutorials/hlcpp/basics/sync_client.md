# Implement a FIDL client

## Prerequisites

This tutorial builds on the [FIDL server][server-tut] tutorial. For the
full set of FIDL tutorials, refer to the [overview][overview].

## Overview

This tutorial implements a client for a FIDL protocol and runs it
against the server created in the [previous tutorial][server-tut]. The client in this
tutorial is synchronous. There is an [alternate tutorial][async-client] for
asynchronous clients.

If you want to write the code yourself, delete the following directories:

```posix-terminal
rm -r examples/fidl/hlcpp/client_sync/*
```

## Create the component

Create a new component project at `examples/fidl/hlcpp/client_sync`:

1. Add a `main()` function to `examples/fidl/hlcpp/client_sync/main.cc`:

   ```cpp
   int main(int argc, const char** argv) {
     printf("Hello, world!\n");
     return 0;
   }
   ```

1. Declare a target for the client in `examples/fidl/hlcpp/client_sync/BUILD.gn`:

   ```gn
   {%includecode gerrit_repo="fuchsia/fuchsia" gerrit_path="examples/fidl/hlcpp/client_sync/BUILD.gn" region_tag="imports" %}

   # Declare an executable for the client.
   executable("bin") {
     output_name = "fidl_echo_hlcpp_client_sync"
     sources = [ "main.cc" ]
   }

   {%includecode gerrit_repo="fuchsia/fuchsia" gerrit_path="examples/fidl/hlcpp/client_sync/BUILD.gn" region_tag="rest" %}
   ```

1. Add a component manifest in `examples/fidl/hlcpp/client_sync/meta/client.cml`:

   Note: The binary name in the manifest must match the output name of the
   `executable` defined in the previous step.

   ```json5
   {%includecode gerrit_repo="fuchsia/fuchsia" gerrit_path="examples/fidl/hlcpp/client_sync/meta/client.cml" region_tag="example_snippet" %}
   ```

1. Once you have created your component, ensure that you can add it to the
   build configuration:

   ```posix-terminal
   fx set core.qemu-x64 --with //examples/fidl/hlcpp/client_sync:echo-client
   ```

1. Build the Fuchsia image:

   ```posix-terminal
   fx build
   ```

## Edit GN dependencies

1. Add the following dependencies:

   ```gn
   {%includecode gerrit_repo="fuchsia/fuchsia" gerrit_path="examples/fidl/hlcpp/client_sync/BUILD.gn" region_tag="deps" %}
   ```

1. Then, include them in `main.cc`:

   ```cpp
   {%includecode gerrit_repo="fuchsia/fuchsia" gerrit_path="examples/fidl/hlcpp/client_sync/main.cc" region_tag="includes" %}
   ```

   The reason for including these dependencies is explained in the
   [server tutorial][server-tut-deps].

## Connect to the server

This section adds code the `main()` function that connects to the server and makes
requests to it.

### Initialize a proxy class

The code then creates a proxy class for the `Echo` protocol, and connects it
to the server. In the context of FIDL, proxy designates the code
generated by the FIDL bindings that enables users to make
remote procedure calls to the server. In HLCPP, the proxy takes the form
of a class with methods corresponding to each FIDL protocol method.

```cpp
{%includecode gerrit_repo="fuchsia/fuchsia" gerrit_path="examples/fidl/hlcpp/client_sync/main.cc" region_tag="main" highlight="2,3,4" %}
```

* [`fuchsia::examples::EchoSyncPtr`][proxy] is an alias for
  `fidl::SynchronousInterfaceRequest<fuchsia::examples::Echo>` generated by the bindings.
  This class will proxy requests for the `Echo` protocol over the channel that
  it is bound to.
* The code calls `EchoSyncPtr::NewRequest()`, which will create a channel, bind the class to
  one end, and return the other end
* The returned end is passed to `sys::ServiceDirectory::Connect()`.
  * Analogous to the call to `context->out()->AddPublicService()` on the server
    side, `Connect` has an implicit second parameter here, which is the protocol
    name (`"fuchsia.examples.Echo"`). This is where the input to the handler
    defined in the [previous tutorial][server-tut-handler] comes from: the
    client passes it in to `Connect`, which then passes it to the handler.

An important point to note here is that this code assumes that `/svc` already
contains an instance of the `Echo` protocol. This is not the case by default
because of the sandboxing provided by the component framework.

### Send requests to the server

The code makes two requests to the server:

* An `EchoString` request
* A `SendString` request

```cpp
{%includecode gerrit_repo="fuchsia/fuchsia" gerrit_path="examples/fidl/hlcpp/client_sync/main.cc" region_tag="main" highlight="6,7,8" %}
```

For `EchoString` the code passes in a pointer for each response parameter (in
this case, the `EchoString` method only has one response parameter), which is
written with the response from the server, whereas this does not apply to
`SendString` since it is a [fire and forget method][one-way]. The call to
`EchoString` will block until it receives a message from the server. Both methods
will return a `zx_status_t` indicating the result of the method call.

Though the [server implementation][server-tut-impl] sends an `OnString` event
in response to the `SendString` request, the sync bindings do not provide a
way to handle this event.

## Run the client

In order for the client and server to communicate using the `Echo` protocol,
component framework must route the `fuchsia.examples.Echo` capability from the
server to the client. For this tutorial, a [realm][glossary.realm] component is
provided to declare the appropriate capabilities and routes.

Note: You can explore the full source for the realm component at
[`//examples/fidl/echo-realm`](/examples/fidl/echo-realm)

1. Configure your build to include the provided package that includes the
   echo realm, server, and client:

    ```posix-terminal
    fx set core.qemu-x64 --with //examples/fidl/hlcpp:echo-hlcpp-client-sync
    ```

1. Build the Fuchsia image:

   ```posix-terminal
   fx build
   ```

1. Run the `echo_realm` component. This creates the client and server component
   instances and routes the capabilities:

    ```posix-terminal
    ffx component run fuchsia-pkg://fuchsia.com/echo-hlcpp-client-sync#meta/echo_realm.cm
    ```

1. Start the `echo_client` instance:

    ```posix-terminal
    ffx component bind /core/ffx-laboratory:echo_realm/echo_client
    ```

The server component starts when the client attempts to connect to the `Echo`
protocol. You should see the following output using `fx log`:

```none {:.devsite-disable-click-to-copy}
[echo_server] INFO: Running echo server
[echo_client] INFO: Got response: hello
```

Terminate the realm component to stop execution and clean up the component
instances:

```posix-terminal
ffx component destroy /core/ffx-laboratory:echo_realm
```

<!-- xrefs -->
[glossary.realm]: /docs/glossary/README.md#realm
[client-tut-main]: /docs/development/languages/fidl/tutorials/hlcpp/client.md#proxy
[server-tut]: /docs/development/languages/fidl/tutorials/hlcpp/basics/server.md
[server-tut-component]: /docs/development/languages/fidl/tutorials/hlcpp/basics/server.md#component
[server-tut-impl]: /docs/development/languages/fidl/tutorials/hlcpp/basics/server.md#impl
[server-tut-deps]: /docs/development/languages/fidl/tutorials/hlcpp/basics/server.md#dependencies
[server-tut-handler]: /docs/development/languages/fidl/tutorials/hlcpp/basics/server.md#handler
[async-client]: /docs/development/languages/fidl/tutorials/hlcpp/basics/client.md
[proxy]: /docs/reference/fidl/bindings/hlcpp-bindings.md#protocols-client
[overview]: /docs/development/languages/fidl/tutorials/overview.md
[environment]: /docs/concepts/components/v2/environments.md
