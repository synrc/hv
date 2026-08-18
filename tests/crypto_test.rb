#!/usr/bin/env ruby
# frozen_string_literal: true

require 'pty'
require 'timeout'

$stdout.sync = true
$stderr.sync = true

puts "Starting Erlang seL4 crypto verification test via PTY..."

success = false
hash_verified = false

begin
  PTY.spawn("make run") do |stdout, stdin, pid|
    begin
      Timeout.timeout(45) do
        buffer = ""
        # Read character by character to detect prompt in real-time
        stdout.each_char do |char|
          print char # Print output in real-time (unbuffered)
          buffer += char
          
          # Eshell prompt detection
          if buffer.include?("1> ")
            stdin.puts("application:which_applications().")
            buffer = ""
          elsif buffer.include?("2> ")
            if buffer.include?("crypto")
              puts "\n[SUCCESS] crypto is started automatically in boot script!"
              success = true
              stdin.puts("crypto:hash(md5, <<\"test\">>).")
            else
              puts "\n[WARNING] crypto was not started automatically. Attempting manual start..."
              stdin.puts("application:start(crypto).")
            end
            buffer = ""
          elsif buffer.include?("3> ")
            if success
              # Started automatically: prompt 3> shows the result of the hash
              if buffer.include?("<<9,143,107,205,70,33,211,115,202,222,78,131,38,39,180,246>>")
                puts "\n[SUCCESS] crypto:hash/2 works natively on seL4!"
                hash_verified = true
              else
                puts "\n[FAILURE] crypto:hash/2 failed or returned incorrect result."
              end
              stdin.puts("init:stop().")
              break
            else
              # Manually started: prompt 3> shows application:start output. Send hash command now.
              stdin.puts("crypto:hash(md5, <<\"test\">>).")
            end
            buffer = ""
          elsif buffer.include?("4> ")
            if buffer.include?("<<9,143,107,205,70,33,211,115,202,222,78,131,38,39,180,246>>")
              puts "\n[SUCCESS] crypto:hash/2 works natively on seL4!"
              hash_verified = true
            else
              puts "\n[FAILURE] crypto:hash/2 failed or returned incorrect result."
            end
            stdin.puts("init:stop().")
            break
          end
        end
      end
    rescue Timeout::Error
      puts "\n[FAILURE] Test timed out waiting for Erlang shell."
    rescue Errno::EIO
      # Process finished/closed its stdout
    ensure
      # Force kill QEMU pid
      Process.kill("KILL", pid) rescue nil
    end
  end
rescue PTY::ChildExited => e
  puts "QEMU process exited: #{e.status}"
end

if success && hash_verified
  puts "Erlang/seL4 crypto test PASSED!"
  exit 0
else
  puts "Erlang/seL4 crypto test FAILED!"
  exit 1
end
