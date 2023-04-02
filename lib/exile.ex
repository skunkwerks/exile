defmodule Exile do
  @moduledoc """
  Exile is an alternative for beam [ports](https://hexdocs.pm/elixir/Port.html)
  with back-pressure and non-blocking IO.

  ## Comparison with Port

    * it is demand driven. User explicitly has to `read` the command
  output, and the progress of the external command is controlled
  using OS pipes. Exile never load more output than we can consume,
  so we should never experience memory issues

    * it can close stdin while consuming output

    * tries to handle zombie process by attempting to cleanup
  external process. Note that there is no middleware involved
  with exile so it is still possible to endup with zombie process.

    * selectively consume stdout and stderr

  Internally Exile uses non-blocking asynchronous system calls
  to interact with the external process. It does not use port's
  message based communication, instead uses raw stdio and NIF.
  Uses asynchronous system calls for IO. Most of the system
  calls are non-blocking, so it should not block the beam
  schedulers. Make use of dirty-schedulers for IO
  """

  use Application

  @doc false
  def start(_type, _args) do
    opts = [
      name: Exile.WatcherSupervisor,
      strategy: :one_for_one
    ]

    # We use DynamicSupervisor for cleaning up external processes on
    # :init.stop or SIGTERM
    DynamicSupervisor.start_link(opts)
  end

  @doc """
  Runs the command with arguments and return an Enumerable to read the output.

  First parameter must be a list containing command with arguments.
  example: `["cat", "file.txt"]`.

  ### Options

    * `input` - Input can be either an `Enumerable` or a function which accepts `Collectable`.

      * Enumerable:

        ```
        # List
        Exile.stream!(~w(base64), input: ["hello", "world"]) |> Enum.to_list()
        # Stream
        Exile.stream!(~w(cat), input: File.stream!("log.txt", [], 65_536)) |> Enum.to_list()
        ```

      * Collectable:

        If the input in a function with arity 1, Exile will call that function
        with a `Collectable` as the argument. The function must *push* input to this
        collectable. Return value of the function is ignored.

        ```
        Exile.stream!(~w(cat), input: fn sink -> Enum.into(1..100, sink, &to_string/1) end)
        |> Enum.to_list()
        ```

        By defaults no input is sent to the command

    * `exit_timeout` - Duration to wait for external program to exit after completion
  before raising an error. Defaults to `:infinity`

    * `max_chunk_size` - Maximum size of iodata chunk emitted by the stream.
  Chunk size can be less than the `max_chunk_size` depending on the amount of
  data available to be read. Defaults to `65_535`

    * `enable_stderr` - When set to true, output stream will contain stderr data along
  with stdout. Stream data will be of the form `{:stdout, iodata}` or `{:stderr, iodata}`
  to differentiate different streams. Defaults to false. See example below

    * `ignore_epipe` - when set to true, `EPIPE` error during the write will be ignored.
  This can be used to match UNIX shell default behaviour. EPIPE is the error raised
  when the reader finishes the reading and close output pipe before command completes.
  Defaults to `false`.

  Remaining options are passed to `Exile.Process.start_link/2`

  ### Examples

  ```
  Exile.stream!(~w(ffmpeg -i pipe:0 -f mp3 pipe:1), input: File.stream!("music_video.mkv", [], 65_535))
  |> Stream.into(File.stream!("music.mp3"))
  |> Stream.run()
  ```

  Stream with stderr

  ```
  Exile.stream!(~w(ffmpeg -i pipe:0 -f mp3 pipe:1),
    input: File.stream!("music_video.mkv", [], 65_535),
    enable_stderr: true
  )
  |> Stream.transform(
    fn ->
      File.open!("music.mp3", [:write, :binary])
    end,
    fn elem, file ->
      case elem do
        {:stdout, data} ->
          :ok = IO.binwrite(file, data)

        {:stderr, msg} ->
          :ok = IO.write(msg)
      end

      {[], file}
    end,
    fn file ->
      :ok = File.close(file)
    end
  )
  |> Stream.run()
  ```
  """
  @type collectable_func() :: (Collectable.t() -> any())

  @spec stream!(nonempty_list(String.t()),
          input: Enum.t() | collectable_func(),
          exit_timeout: timeout(),
          max_chunk_size: pos_integer()
        ) :: Exile.Stream.t()
  def stream!(cmd_with_args, opts \\ []) do
    Exile.Stream.__build__(cmd_with_args, opts)
  end
end
