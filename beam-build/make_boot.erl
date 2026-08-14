-module(make_boot).
-export([main/1]).

main([StagingDir, AppsConfig]) ->
    {ok, [{apps, AppNames}]} = file:consult(AppsConfig),

    AppsDataRaw = lists:map(fun(App) ->
        AppDir = filename:join([StagingDir, "otp", "lib", atom_to_list(App)]),
        AppFile = filename:join([AppDir, "ebin", atom_to_list(App) ++ ".app"]),
        case file:consult(AppFile) of
            {ok, [{application, App, Opts}]} -> {App, Opts};
            _ -> false
        end
    end, AppNames),

    AppsData = [X || X <- AppsDataRaw, X =/= false],

    KernelOptsRaw = proplists:get_value(kernel, AppsData),
    KernelEnv = proplists:get_value(env, KernelOptsRaw, []),
    KernelEnv1 = lists:keyreplace(error_logger, 1, KernelEnv, {error_logger, silent}),
    KernelOpts = lists:keyreplace(env, 1, KernelOptsRaw, {env, KernelEnv1}),

    StdlibOpts = proplists:get_value(stdlib, AppsData),

    % Core lists
    KernelMods = proplists:get_value(modules, KernelOpts),
    StdlibMods = proplists:get_value(modules, StdlibOpts),

    PreLoaded = [erl_prim_loader,erl_tracer,erlang,erts_code_purger,
                 erts_dirty_process_code_checker,erts_internal,
                 erts_literal_area_collector,init,otp_ring0,prim_eval,prim_file,
                 prim_inet,prim_zip,zlib],

    Path = fun(A) -> 
        "$ROOT/lib/" ++ atom_to_list(A) ++ "/ebin"
    end,

    Paths = [{path, [Path(A) || {A, _} <- AppsData]}],

    CoreLoad = [error_handler,application,application_controller,
                application_master,code,code_server,erl_eval,erl_lint,erl_parse,
                error_logger,ets,file,filename,file_server,file_io_server,
                gen,gen_event,gen_server,heart,kernel,lists,proc_lib,supervisor],

    Insts = [
        {preLoaded, PreLoaded},
        {progress, preloaded}
    ] ++ Paths ++ [
        {primLoad, CoreLoad},
        {kernel_load_completed},
        {progress, kernel_load_completed},
        {path, [Path(kernel)]},
        {primLoad, [M || M <- KernelMods, not lists:member(M, CoreLoad)]}
    ] ++ [{path, [Path(stdlib)]}, {primLoad, [M || M <- StdlibMods, not lists:member(M, CoreLoad)]}]
      ++ lists:flatten([ [{path, [Path(App)]}, {primLoad, proplists:get_value(modules, Opts)}]
                      || {App, Opts} <- AppsData, App =/= kernel, App =/= stdlib ])
      ++ [{progress, modules_loaded}] ++ Paths ++ [
        {kernelProcess, error_logger, {error_logger, start_link, []}},
        {kernelProcess, application_controller, {application_controller, start, [{application, kernel, KernelOpts}]}},
%        {kernelProcess, user, {user, start, []}},
        {progress, init_kernel_started},
        {apply, {application, load, [{application, stdlib, StdlibOpts}]}}
    ] ++ lists:flatten([
        [{apply, {application, load, [{application, App, Opts}]}} || {App, Opts} <- AppsData, App =/= kernel, App =/= stdlib]
    ]) ++ [
        {progress, applications_loaded},
        {apply, {erlang, display, [<<"OS.1 BEAM erlang booloader script start.boot @ seL4/Microkit">>]} }
    ] ++ lists:flatten([
        [{apply, {application, start_boot, [App, permanent]}} || App <- [ kernel, stdlib, compiler, syntax_tools, parsetools, asn1 ] ]
    ]) ++ [
%        {apply, {c, erlangrc, []}},
        {progress, started}
    ],

    Script = {script, {"OS.1 Hypervisor", "1.0"}, Insts},
    file:write_file(filename:join([StagingDir, "otp", "bin", "start.boot"]), term_to_binary(Script)),
    halt().
