#!/usr/bin/env ruby
# frozen_string_literal: true

require 'pty'
require 'timeout'

$stdout.sync = true
$stderr.sync = true

puts "Starting Erlang seL4 crypto & au verification test via PTY..."

success = false
au_logged = false
list_dir_verified = false
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
            has_crypto = buffer.include?("crypto")
            has_au = buffer.include?("au")
            has_mnesia = buffer.include?("mnesia")
            has_pubkey = buffer.include?("public_key")
            has_ssl = buffer.include?("ssl")
            has_ssh = buffer.include?("ssh")
            has_inets = buffer.include?("inets")
            
            if has_crypto && has_au && has_mnesia && has_pubkey && has_ssl && has_ssh && has_inets
              puts "\n[SUCCESS] crypto, au, mnesia, public_key, ssl, ssh, and inets are started automatically in boot script!"
              success = true
              # Send audit log command
              stdin.puts('audit_client:log(application, <<"user_session_101">>, login, <<"/">>, auth, success, #{}).')
            else
              puts "\n[FAILURE] Required applications not started. crypto: #{has_crypto}, au: #{has_au}, mnesia: #{has_mnesia}, public_key: #{has_pubkey}, ssl: #{has_ssl}, ssh: #{has_ssh}, inets: #{has_inets}"
              stdin.puts("init:stop().")
              break
            end
            buffer = ""
          elsif buffer.include?("3> ")
            if buffer.include?("ok")
              puts "\n[SUCCESS] audit_client:log/7 successfully logged event to audit_core!"
              au_logged = true
            else
              puts "\n[FAILURE] audit_client:log/7 failed to log event."
            end
            # Send list_dir verification command
            stdin.puts('file:list_dir("/").')
            buffer = ""
          elsif buffer.include?("4> ")
            if buffer.include?("otp")
              puts "\n[SUCCESS] file:list_dir(\"/\") successfully listed root directory containing otp!"
              list_dir_verified = true
            else
              puts "\n[FAILURE] file:list_dir(\"/\") failed or returned incorrect result."
            end
            # Send hash verification command
            stdin.puts("crypto:hash(md5, <<\"test\">>).")
            buffer = ""
          elsif buffer.include?("5> ")
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

if success && au_logged && list_dir_verified && hash_verified
  puts "Erlang/seL4 crypto & au & list_dir test PASSED!"
  exit 0
else
  puts "Erlang/seL4 crypto & au & list_dir test FAILED!"
  exit 1
end
