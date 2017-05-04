# encoding: binary
#  Phusion Passenger - https://www.phusionpassenger.com/
#  Copyright (c) 2011-2017 Phusion Holding B.V.
#
#  "Passenger", "Phusion Passenger" and "Union Station" are registered
#  trademarks of Phusion Holding B.V.
#
#  Permission is hereby granted, free of charge, to any person obtaining a copy
#  of this software and associated documentation files (the "Software"), to deal
#  in the Software without restriction, including without limitation the rights
#  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
#  copies of the Software, and to permit persons to whom the Software is
#  furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice shall be included in
#  all copies or substantial portions of the Software.
#
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
#  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
#  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
#  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
#  THE SOFTWARE.

module PhusionPassenger

  # Provides shared functions for loader and preloader apps.
  module LoaderSharedHelpers
    extend self

    # To be called by the (pre)loader as soon as possible.
    def init(main_app, step_info)
      @main_app = main_app
      options = read_startup_arguments

      # We don't dump PATH info because at this point it's
      # unlikely to be changed.
      dump_ruby_environment
      check_rvm_using_wrapper_script(options)

      PhusionPassenger.require_passenger_lib 'native_support'
      if defined?(NativeSupport)
        NativeSupport.disable_stdio_buffering
      end

      PhusionPassenger.require_passenger_lib 'constants'
      PhusionPassenger.require_passenger_lib 'public_api'
      PhusionPassenger.require_passenger_lib 'debug_logging'
      PhusionPassenger.require_passenger_lib 'utils/shellwords'
      PhusionPassenger.require_passenger_lib 'ruby_core_enhancements'
      PhusionPassenger.require_passenger_lib 'ruby_core_io_enhancements'
      PhusionPassenger.require_passenger_lib 'request_handler'

      PhusionPassenger.require_passenger_lib 'rack/thread_handler_extension'
      RequestHandler::ThreadHandler.send(:include, Rack::ThreadHandlerExtension)
      Thread.main[:name] = "Main thread"

      options
    rescue Exception => e
      record_journey_step_errored(step_info)
      record_and_print_exception(e)
      exit exit_code_for_exception(e)
    end

    def read_startup_arguments
      work_dir = ENV['PASSENGER_SPAWN_WORK_DIR']
      PhusionPassenger.require_passenger_lib 'utils/json'
      @@options = File.open("#{work_dir}/args.json", 'rb') do |f|
        PhusionPassenger::Utils::JSON.parse(f.read)
      end
    end

    def check_rvm_using_wrapper_script(options)
      ruby = options["ruby"]
      if ruby =~ %r(/\.?rvm/) && ruby =~ %r(/bin/ruby$)
        case options["integration_mode"] || DEFAULT_INTEGRATION_MODE
        when "nginx"
          passenger_ruby = "passenger_ruby"
          passenger_ruby_doc = "https://www.phusionpassenger.com/library/config/nginx/reference/#setting_correct_passenger_ruby_value"
        when "apache"
          passenger_ruby = "PassengerRuby"
          passenger_ruby_doc = "https://www.phusionpassenger.com/library/config/apache/reference/#setting_correct_passenger_ruby_value"
        when "standalone"
          passenger_ruby = "--ruby"
          passenger_ruby_doc = "https://www.phusionpassenger.com/library/config/standalone/reference/#setting_correct_passenger_ruby_value"
        end

        log_error_to_response_dir(
          :summary => "#{passenger_ruby} must be set to an RVM wrapper script instead of a raw Ruby binary",

          :problem_description_html =>
            "You've set the <code>#{h passenger_ruby}</code> option to <code>#{h ruby}</code>. " \
            'However, because you are using RVM, this is not allowed: the option must point to ' \
            'an RVM wrapper script, not a raw Ruby binary. This is because RVM is implemented ' \
            "through various environment variables, which are set through the wrapper script.\n",

          :solution_description_html =>
            "To find out the correct value for <code>#{h passenger_ruby}</code>, please read " \
            "<a href=\"#{h passenger_ruby_doc}\">its documentation entry</a>."
        )
        abort
      end
    end

    def is_ruby_program?(path)
      File.open(path, "rb") do |f|
        f.readline =~ /ruby/
      end
    rescue EOFError
      false
    end

    def exit_code_for_exception(e)
      if e.is_a?(SystemExit)
        e.status
      else
        1
      end
    end

    # Prepare an application process using rules for the given spawn options.
    # This method is to be called before loading the application code.
    #
    # +startup_file+ is the application type's startup file, e.g.
    # "config/environment.rb" for Rails apps and "config.ru" for Rack apps.
    # +options+ are the spawn options that were given.
    #
    # This function may modify +options+. The modified options are to be
    # passed to the request handler.
    def before_loading_app_code_step1(startup_file, options)
      DebugLogging.log_level = options["log_level"] if options["log_level"]

      # We always load the union_station_hooks_* gems and do not check for
      # `options["analytics_support"]` here. The gems don't actually initialize (and
      # load the bulk of their code) unless they have determined that
      # `options["analytics_support"]` is true. Regardless of whether Union Station
      # support is enabled in Passenger, the UnionStationHooks namespace must
      # be available so that applications can call it, even though the actual
      # calls don't do anything when Union Station support is disabled.
      PhusionPassenger.require_passenger_lib 'vendor/union_station_hooks_core/lib/union_station_hooks_core'
      UnionStationHooks.vendored = true
      PhusionPassenger.require_passenger_lib 'vendor/union_station_hooks_rails/lib/union_station_hooks_rails'
      UnionStationHooksRails.vendored = true
    end

    def run_load_path_setup_code(options)
      # rack-preloader.rb depends on the 'rack' library, but the app
      # might want us to use a bundled version instead of a
      # gem/apt-get/yum/whatever-installed version. Therefore we must setup
      # the correct load paths before requiring 'rack'.
      #
      # The most popular tool for bundling dependencies is Bundler. Bundler
      # works as follows:
      # - If the bundle is locked then a file .bundle/environment.rb exists
      #   which will setup the load paths.
      # - If the bundle is not locked then the load paths must be set up by
      #   calling Bundler.setup.
      # - Rails 3's boot.rb automatically loads .bundle/environment.rb or
      #   calls Bundler.setup if that's not available.
      # - Other Rack apps might not have a boot.rb but we still want to setup
      #   Bundler.
      # - Some Rails 2 apps might have explicitly added Bundler support.
      #   These apps call Bundler.setup in their preinitializer.rb.
      #
      # So the strategy is as follows:

      # Our strategy might be completely unsuitable for the app or the
      # developer is using something other than Bundler, so we let the user
      # manually specify a load path setup file.
      if options["load_path_setup_file"]
        require File.expand_path(options["load_path_setup_file"])

      # The app developer may also override our strategy with this magic file.
      elsif File.exist?('config/setup_load_paths.rb')
        require File.expand_path('config/setup_load_paths')

      # Older versions of Bundler use .bundle/environment.rb as the Bundler
      # environment lock file. This has been replaced by Gemfile.lock in later
      # versions, but we still support the older mechanism.
      # If the Bundler environment lock file exists then load that. If it
      # exists then there's a 99.9% chance that loading it is the correct
      # thing to do.
      elsif File.exist?('.bundle/environment.rb')
        running_bundler(options) do
          require File.expand_path('.bundle/environment')
        end

      # If the legacy Bundler environment file doesn't exist then there are two
      # possibilities:
      # 1. Bundler is not used, in which case we don't have to do anything.
      # 2. Bundler *is* used, but either the user is using a newer Bundler versions,
      #    or the gems are not locked. In either case, we're supposed to call
      #    Bundler.setup.
      #
      # The existence of Gemfile indicates whether (2) is true:
      elsif File.exist?('Gemfile')
        # In case of Rails 3, config/boot.rb already calls Bundler.setup.
        # However older versions of Rails may not so loading boot.rb might
        # not be the correct thing to do. To be on the safe side we
        # call Bundler.setup ourselves; calling Bundler.setup twice is
        # harmless. If this isn't the correct thing to do after all then
        # there's always the load_path_setup_file option and
        # setup_load_paths.rb.
        running_bundler(options) do
          activate_gem 'bundler', 'bundler/setup'
        end
      end


      # !!! NOTE !!!
      # If the app is using Bundler then any dependencies required past this
      # point must be specified in the Gemfile. Like ruby-debug if debugging is on...
    end

    # This method is to be called after the load path has been set up
    # (e.g. Bundler.setup is called), but before loading the app code.
    def before_loading_app_code_step2(options)
      # Do nothing
    end

    # This method is to be called after loading the application code but
    # before forking a worker process.
    def after_loading_app_code(options)
      UnionStationHooks.check_initialized
    end

    # If the current working directory equals `app_root`, and `abs_path` is a
    # file inside `app_root`, then returns its basename. Otherwise, returns
    # `abs_path`.
    #
    # The main use case for this method is to ensure that we load config.ru
    # with a relative path (only its base name) in most circumstances,
    # instead of with an absolute path. This is necessary in order to retain
    # compatibility with apps that expect config.ru's __FILE__ to be relative.
    # See https://github.com/phusion/passenger/issues/1596
    def maybe_make_path_relative_to_app_root(app_root, abs_path)
      if Dir.logical_pwd == app_root && File.dirname(abs_path) == app_root
        File.basename(abs_path)
      else
        abs_path
      end
    end

    def create_socket_address(protocol, address)
      if protocol == 'unix'
        "unix:#{address}"
      elsif protocol == 'tcp'
        "tcp://#{address}"
      else
        raise ArgumentError, "Unknown protocol '#{protocol}'"
      end
    end

    def advertise_sockets(options, request_handler)
      json = { :sockets => [] }
      request_handler.server_sockets.each_pair do |name, options|
        concurrency = PhusionPassenger.advertised_concurrency_level || options[:concurrency]
        json[:sockets] << {
          :name => name,
          :address => options[:address],
          :protocol => options[:protocol],
          :concurrency => concurrency
        }
      end

      File.open(ENV['PASSENGER_SPAWN_WORK_DIR'] + '/response/properties.json', 'w') do |f|
        f.write(PhusionPassenger::Utils::JSON.generate(json))
      end
    end

    def advertise_readiness(options)
      File.open(ENV['PASSENGER_SPAWN_WORK_DIR'] + '/response/finish', 'w') do |f|
        f.write('1')
      end
    end

    # To be called before the request handler main loop is entered, but after the app
    # startup file has been loaded. This function will fire off necessary events
    # and perform necessary preparation tasks.
    #
    # +forked+ indicates whether the current worker process is forked off from
    # an ApplicationSpawner that has preloaded the app code.
    # +options+ are the spawn options that were passed.
    def before_handling_requests(forked, options)
      if forked
        # Reseed pseudo-random number generator for security reasons.
        srand
      end

      if options["process_title"] && !options["process_title"].empty?
        $0 = options["process_title"] + ": " + options["app_group_name"]
      end

      # If we were forked from a preloader process then clear or
      # re-establish ActiveRecord database connections. This prevents
      # child processes from concurrently accessing the same
      # database connection handles.
      if forked && defined?(ActiveRecord::Base)
        if ActiveRecord::Base.respond_to?(:clear_all_connections!)
          ActiveRecord::Base.clear_all_connections!
        elsif ActiveRecord::Base.respond_to?(:clear_active_connections!)
          ActiveRecord::Base.clear_active_connections!
        elsif ActiveRecord::Base.respond_to?(:connected?) &&
              ActiveRecord::Base.connected?
          ActiveRecord::Base.establish_connection
        end
      end

      # Fire off events.
      PhusionPassenger.call_event(:starting_worker_process, forked)
      if options["pool_account_username"] && options["pool_account_password_base64"]
        password = options["pool_account_password_base64"].unpack('m').first
        PhusionPassenger.call_event(:credentials,
          options["pool_account_username"], password)
      else
        PhusionPassenger.call_event(:credentials, nil, nil)
      end
    end

    # To be called after the request handler main loop is exited. This function
    # will fire off necessary events perform necessary cleanup tasks.
    def after_handling_requests
      PhusionPassenger.call_event(:stopping_worker_process)
    end

    # Activate a gem and require it. This method exists in order to load
    # a library from RubyGems instead of from vendor_ruby. For example,
    # on Debian systems, Rack may be installed from APT, but that is usually
    # a very old version which we don't want. This method ensures that the
    # RubyGems-installed version is loaded, not the the version in vendor_ruby.
    # See the following threads for discussion:
    # https://github.com/phusion/passenger/issues/1478
    # https://github.com/phusion/passenger/issues/1480
    def activate_gem(gem_name, library_name = nil)
      if !defined?(::Gem)
        begin
          require 'rubygems'
        rescue LoadError
        end
      end
      if Kernel.respond_to?(:gem, true)
        begin
          gem(gem_name)
        rescue Gem::LoadError
        end
      end
      require(library_name || gem_name)
    end


    ##### Journey recording #####

    def record_journey_step_in_progress(step)
      @main_app.record_journey_step_in_progress(step)
    end

    def record_journey_step_complete(info, state)
      dir = ENV['PASSENGER_SPAWN_WORK_DIR']

      path = "#{dir}/response/steps/#{info[:step].downcase}/state"
      try_write_file(path, state)

      path = "#{dir}/response/steps/#{info[:step].downcase}/duration"
      duration = Time.now - info[:begin_time]
      try_write_file(path, (duration * 1_000_000).to_s)
    end

    def record_journey_step_performed(info)
      record_journey_step_complete(info, 'STEP_PERFORMED')
    end

    def record_journey_step_errored(info)
      record_journey_step_complete(info, 'STEP_ERRORED')
    end

    def run_block_and_record_step_progress(step)
      step_info = record_journey_step_in_progress(step)
      begin
        yield
      rescue Exception => e
        record_journey_step_errored(step_info)
        raise e
      end
      record_journey_step_performed(step_info)
    end


    ##### Error reporting #####

    # To be called whenever the (pre)loader is about to abort with an error.
    def about_to_abort(options, exception)
      dump_all_information(options)
    end

    def record_and_print_exception(e)
      record_error_category_based_on_exception(e)
      record_and_print_error_summary(
        "The application encountered the following error: #{e} (#{e.class})")
      STDERR.write("    #{e.backtrace.join("\n    ")}\n")
      record_advanced_problem_details(format_exception(e))
      if e.respond_to?(:problem_description_html)
        record_problem_description_html(e.problem_description_html)
      end
      if e.respond_to?(:solution_description_html)
        record_solution_description_html(e.solution_description_html)
      end
    end

    def record_error_category(category)
      dir = ENV['PASSENGER_SPAWN_WORK_DIR']
      try_write_file("#{dir}/response/error/category", category)
    end

    def record_error_category_based_on_exception(e)
      if e.is_a?(IOError)
        record_error_category('IO_ERROR')
      elsif e.is_a?(SystemCallError)
        record_error_category('OPERATING_SYSTEM_ERROR')
      else
        record_error_category('INTERNAL_ERROR')
      end
    end

    def record_error_summary(summary)
      dir = ENV['PASSENGER_SPAWN_WORK_DIR']
      try_write_file("#{dir}/response/error/summary", summary)
    end

    def record_and_print_error_summary(summary)
      STDERR.puts "Error: #{summary}"
      record_error_summary(summary)
    end

    def record_advanced_problem_details(message)
      dir = ENV['PASSENGER_SPAWN_WORK_DIR']
      try_write_file("#{dir}/response/error/advanced_problem_details", message)
    end

    def record_problem_description_html(html)
      dir = ENV['PASSENGER_SPAWN_WORK_DIR']
      try_write_file("#{dir}/response/error/problem_description.html", html)
    end

    def record_solution_description_html(html)
      dir = ENV['PASSENGER_SPAWN_WORK_DIR']
      try_write_file("#{dir}/response/error/solution_description.html", html)
    end

    def format_exception(e)
      result = "#{e} (#{e.class})"
      if !e.backtrace.empty?
        result << "\n  " << e.backtrace.join("\n  ")
      end
      result
    end


    ##### Environment dumping #####

    def dump_all_information(options)
      dump_ruby_environment
      dump_envvars
    end

    def dump_ruby_environment
      dir = "#{ENV['PASSENGER_SPAWN_WORK_DIR']}/envdump"

      File.open("#{dir}/ruby_info", "w") do |f|
        f.puts "RUBY_VERSION = #{RUBY_VERSION}"
        f.puts "RUBY_PLATFORM = #{RUBY_PLATFORM}"
        f.puts "RUBY_ENGINE = #{defined?(RUBY_ENGINE) ? RUBY_ENGINE : 'nil'}"
      end
      File.open("#{dir}/load_path", "wb") do |f|
        $LOAD_PATH.each do |path|
          f.puts path
        end
      end
      File.open("#{dir}/loaded_libs", "wb") do |f|
        $LOADED_FEATURES.each do |filename|
          f.puts filename
        end
      end

      # We write to these files last because the 'require' calls can fail.
      require 'rbconfig' if !defined?(RbConfig::CONFIG)
      File.open("#{dir}/rbconfig", "wb") do |f|
        RbConfig::CONFIG.each_pair do |key, value|
          f.puts "#{key} = #{value}"
        end
      end
      begin
        require 'rubygems' if !defined?(Gem)
      rescue LoadError
      end
      if defined?(Gem)
        File.open("#{dir}/ruby_info", "a") do |f|
          f.puts "RubyGems version = #{Gem::VERSION}"
          if Gem.respond_to?(:path)
            f.puts "RubyGems paths = #{Gem.path.inspect}"
          else
            f.puts "RubyGems paths = unknown; incompatible RubyGems API"
          end
        end
        File.open("#{dir}/activated_gems", "wb") do |f|
          if Gem.respond_to?(:loaded_specs)
            Gem.loaded_specs.each_pair do |name, spec|
              f.puts "#{name} => #{spec.version}"
            end
          else
            f.puts "Unable to query this information; incompatible RubyGems API."
          end
        end
      end
    rescue SystemCallError
      # Don't care.
    end

    def dump_envvars
      dir = "#{ENV['PASSENGER_SPAWN_WORK_DIR']}/envdump"
      try_write_file("#{dir}/envvars", ENV.to_a.map { |k, v| "#{k} = #{v}" }.join("\n"))
    end

  private
    def running_bundler(options)
      yield
    rescue Exception => e
      if (defined?(Bundler::GemNotFound) && e.is_a?(Bundler::GemNotFound)) ||
         (defined?(Bundler::GitError) && e.is_a?(Bundler::GitError))
        PhusionPassenger.require_passenger_lib 'platform_info/ruby'
        ruby = options['ruby']
        e_as_str = "#{e}"  # Certain classes like Interrupt don't like #to_s, so we use this
        if Bundler.respond_to?(:settings) && Bundler.settings.respond_to?(:path)
          bundle_path = Bundler.settings.path
        end
        case options['integration_mode']
        when 'apache'
          passenger_ruby = 'PassengerRuby'
          passenger_ruby_doc = 'https://www.phusionpassenger.com/library/config/apache/reference/#passengerruby'
          passenger_user = 'PassengerUser'
          passenger_user_doc = 'https://www.phusionpassenger.com/library/config/apache/reference/#passengeruser'
        when 'nginx'
          passenger_ruby = 'passenger_ruby'
          passenger_ruby_doc = 'https://www.phusionpassenger.com/library/config/nginx/reference/#passenger_ruby'
          passenger_user = 'passenger_user'
          passenger_user_doc = 'https://www.phusionpassenger.com/library/config/nginx/reference/#passenger_user'
        when 'standalone'
          passenger_ruby = '--ruby'
          passenger_ruby_doc
        else
          raise "Unknown integration mode #{options['integration_mode'].inspect}"
        end


        problem_description =
          "<p>Bundler was unable to find one of the gems defined in the Gemfile. Possible" \
          " causes for this problem are:</p>" \
          "<ul>" \
          "<li>You did not install all the gems that this application needs.</li>" \
          "<li>The necessary gems are installed, but the Bundler does not have" \
          " permissions to access them."
        if bundle_path
          problem_description << " Bundler tried to load the gems from #{h bundle_path}."
        end
        problem_description <<
          "</li>" \
          "<li>The application is being run under the wrong Ruby interpreter.</li>"
        if PlatformInfo.in_rvm?
          problem_description <<
            "<li>The application is being run under the wrong RVM gemset.</li>"
        end
        if ruby =~ %r(^/usr/local/rvm/)
          problem_description <<
            "<li>Your gems are installed to <code>#{h home_dir}/.rvm/gems</code>, while at" \
            " the same time you set <a href=\"#{h passenger_ruby_doc}\">#{h passenger_ruby}</a>" \
            " to <code>#{h ruby}</code>. Because of the latter, RVM does not load gems from the " \
            "home directory.</li>"
        end
        if PlatformInfo.in_rvm?
          problem_description <<
            "<li>The RVM gemset may be broken.</li>"
        end
        problem_description <<
          "</ul>" \
          "<h3>Raw Bundler exception</h3>" \
          "<p>Exception message:</p>" \
          "<pre>#{h e_as_str} (#{h e.class.to_s})</pre>" \
          "<p>Backtrace:<p>" \
          "<pre>#{h e.backtrace.join("\n")}</pre>"
        attach_problem_description_html_to_exception(e, problem_description)


        solution_description =
          "<div class=\"multiple-solutions\">" \
          \
          "<h3>Make sure the gem bundle is installed</h3>" \
          "<p>Run the following from the application directory:</p>" \
          "<pre>bundle install</pre>" \
          \
          "<h3>Check the application process's execution environment</h3>" \
          "<p>Is the application running under the expected execution environment?" \
          " A common problem is that the application runs under a different user than" \
          " it is supposed to. The application is currently running as the <code>#{h whoami}</code>" \
          " user &mdash; is this expected? Also, check the 'Details &amp; diagnostics'" \
          " &raquo; 'Subprocess' tab and double check all information there &mdash; is" \
          " everything as expected? If not, please fix that.</p>"
        if passenger_user
          solution_description <<
            "<p>If the application is not supposed to run as <code>#{h whoami}</code>," \
            " then you can configure this via the" \
            " <a href=\"#{h passenger_user_doc}\">#{h passenger_user}</a>" \
            " setting.</p>"
        end
        solution_description <<
          "<h3>Check that the application has permissions to access the directory from which Bundler loads gems</h3>" \
          "<p>Please check whether the application, which is running as the" \
            " <code>#{h whoami}</code> user, has permissions to access"
        if bundle_path
          solution_description <<
            " <code>#{h bundle_path}</code>."
        else
          solution_description <<
            " the directory that Bundler tries to load gems from. Unfortunately" \
            " #{SHORT_PROGRAM_NAME} was unable to figure out which directory this"
            " is because Bundler is too old, so you need to figure out the" \
            " directory yourself (or you can upgrade Bundler so that #{SHORT_PROGRAM_NAME}" \
            " can figure out the path for you)."
        end
        solution_description <<
          "</p>" \
          \
          "<h3>Check whether the application is being run under the correct Ruby interpreter</h3>" \
          "<p>Is the application supposed to be run with <code>#{h ruby}</code>?" \
          " If not, please change the <a href=\"#{h passenger_ruby_doc}\">#{h passenger_ruby}</a>" \
          " setting.</p>"
        if PlatformInfo.in_rvm?
          solution_description <<
            "<h3>Check whether the application is being run under the correct RVM gemset</h3>" \
            "<p>Is the application supposed to run under the <code>#{h PlatformInfo.rvm_ruby_string}</code>"
            " gemset? If not, please change the <a href=\"#{h passenger_ruby_doc}\">#{h passenger_ruby}</a>" \
            " setting. The documentation for that setting will teach you how to refer" \
            " to the proper gemset.</p>"
        end
        if ruby =~ %r(^/usr/local/rvm/)
          solution_description <<
            "<h3>Is your gem bundle installed to the home directory, while at the same" \
            " time you are using a Ruby that is installed by RVM in a system-wide manner?</h3>" \
            "<p>Your Ruby interpreter is installed by RVM in a system-wide manner: it is" \
            " located in #{h ruby}. If Bundler tries to load gems from " \
            "<code>#{h home_dir}</code>/.rvm/gems, then that won't work.</p>" \
            "<p>To make Bundler and RVM able to load gems from the home directory, set " \
            "<a href=\"#{h passenger_ruby_doc}\">#{h passenger_ruby}</a> to an RVM wrapper script " \
            "inside the home directory:</p>\n\n" \
            "<ol>\n" \
            "  <li>Login as #{h whoami}.</li>\n"
          if PlatformInfo.rvm_installation_mode == :multi
            solution_description <<
              "  <li>Enable RVM mixed mode by running:\n" \
              "      <pre>rvm user gemsets</pre></li>\n"
          end
          solution_description <<
            "  <li>Run this to find out what to set" \
            "      <a href=\"#{h passenger_ruby_doc}\">#{h passenger_ruby}</a> to:\n" \
            "      <pre>#{h PlatformInfo.ruby_command} \\\n" \
            "#{PhusionPassenger.bin_dir}/passenger-config --detect-ruby</pre></li>\n" \
            "</ol>\n\n"
        end
        if PlatformInfo.in_rvm?
          solution_description <<
            "<h3>Reset your RVM gemset</h3>"
            "<p>Sometimes, RVM gemsets maybe be broken." \
            "<a href=\"https://github.com/phusion/passenger/wiki/Resetting-RVM-gemsets\">" \
            "Try resetting them.</a></p>"
        end
        attach_solution_description_html_to_exception(e, solution_description)
      end
      raise e
    end

    def attach_problem_description_html_to_exception(e, html)
      metaclass = class << e; self; end
      metaclass.send(:define_method, :problem_description_html) do
        html
      end
    end

    def attach_solution_description_html_to_exception(e, html)
      metaclass = class << e; self; end
      metaclass.send(:define_method, :problem_description_html) do
        html
      end
    end

    def try_write_file(path, contents)
      begin
        File.open(path, 'wb') do |f|
          f.write(contents)
        end
      rescue SystemCallError => e
        STDERR.puts "Warning: unable to write to #{path}: #{e}"
      end
    end

    def h(text)
      require 'erb' if !defined?(ERB)
      ERB::Util.h(text)
    end

    def whoami
      require 'etc' if !defined?(Etc)
      begin
        user = Etc.getpwuid(Process.uid)
      rescue ArgumentError
        user = nil
      end
      if user
        user.name
      else
        "##{Process.uid}"
      end
    end

    def home_dir
      PhusionPassenger.home_dir
    end
  end

end # module PhusionPassenger
