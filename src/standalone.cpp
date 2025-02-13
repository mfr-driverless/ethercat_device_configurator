/*
 ** Copyright 2021 Robotic Systems Lab - ETH Zurich:
 ** Lennart Nachtigall, Jonas Junger
 ** Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
 **
 ** 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 **
 ** 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
 **
 ** 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.
 **
 ** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


/*
** Simple Example executable for the use of the RSL EtherCAT software tools
** ════════════════════════════════════════════════════════════════════════
**
**   To understand the logic it is best to start at the main function and
**   then go through the code step by step. The executable compiled from
**   this source code needs to be executed as root. Simply executing the
**   executable with sudo will not work because the catkin workspace won’t
**   be sourced and the linker cannot find the necessary libraries. Use the
**   following command to start this executable:
**   ┌────
**   │ sudo bash -c 'source /absolute/path/to/your/catkin_ws/devel/setup.bash; path/to/executable path/to/setup.yaml'
**   └────
**
**
** Build errors
** ────────────
**
**   Are you using gcc and g++ both with a version >= 8.4? See the
**   README.md for more details.
 */

#include <icecream.hpp>

#include "ethercat_device_configurator/EthercatDeviceConfigurator.hpp"

#include <maxon_epos_ethercat_sdk/Maxon.hpp>
#include <thread>
#include <csignal>


std::unique_ptr<std::thread> worker_thread;
bool abrt = false;

EthercatDeviceConfigurator::SharedPtr configurator;

unsigned int counter = 0;

void worker()
{
    bool rtSuccess = true;
    for(const auto & master: configurator->getMasters())
    {
        rtSuccess &= master->setRealtimePriority(99);
    }
    std::cout << "Setting RT Priority: " << (rtSuccess? "successful." : "not successful. Check user privileges.") << std::endl;

    // Flag to set the drive state for the maxon on first startup
    bool maxonEnabledAfterStartup = false;
    // bool maxonOperational = false;

    int target = 0;

    /*
    ** The communication update loop.
    ** This loop is supposed to be executed at a constant rate.
    ** The EthercatMaster::update function incorporates a mechanism
    ** to create a constant rate.
     */
    while(!abrt)
    {
        /*
        ** Update each master.
        ** This sends tha last staged commands and reads the latest readings over EtherCAT.
        ** The StandaloneEnforceRate update mode is used.
        ** This means that average update rate will be close to the target rate (if possible).
         */
        for(const auto & master: configurator->getMasters() )
        {
            master->update(ecat_master::UpdateMode::StandaloneEnforceRate); // TODO fix the rate compensation (Elmo reliability problem)!!
        }

        /*
        ** Do things with the attached devices.
        ** Your lowlevel control input / measurement logic goes here.
        ** Different logic can be implemented for each device.
         */
        for(const auto & slave:configurator->getSlaves())
        {
            // Maxon
            if (configurator->getInfoForSlave(slave).type == EthercatDeviceConfigurator::EthercatSlaveType::Maxon)
            {
                // Keep constant update rate
                // auto start_time = std::chrono::steady_clock::now();

                std::shared_ptr<maxon::Maxon> maxon_slave_ptr = std::dynamic_pointer_cast<maxon::Maxon>(slave);

                if (!maxonEnabledAfterStartup)
                {
                    // Set maxons to operation enabled state, do not block the call!
                    maxon_slave_ptr->setDriveStateViaPdo(maxon::DriveState::OperationEnabled, false);
                }

                // set commands if we can
                if (maxon_slave_ptr->lastPdoStateChangeSuccessful() &&
                        maxon_slave_ptr->getReading().getDriveState() == maxon::DriveState::OperationEnabled)
                {
                    maxon::Command command;
                    command.setModeOfOperation(maxon::ModeOfOperationEnum::CyclicSynchronousPositionMode);
                    //command.setModeOfOperation(maxon::ModeOfOperationEnum::CyclicSynchronousTorqueMode);
                    //command.setModeOfOperation(maxon::ModeOfOperationEnum::CyclicSynchronousVelocityMode);

                    auto reading = maxon_slave_ptr->getReading();
                    MELO_DEBUG_STREAM("ACTUAL POSITION '" << reading.getActualPosition());

                    //command.setTargetPosition(reading.getActualPosition() + 1);
                    auto eps = 0.00000000001;
                    auto position = reading.getActualPosition();
                    if (position < 0 + eps) {
                        target = 360;
                    } else if (position >= 360 - eps) {
                        target = 0;
                    }

                    command.setTargetPosition(target);
                    //command.setTargetVelocity(0.001);
                    //command.setTargetTorque(-0.5);

                    maxon_slave_ptr->stageCommand(command);
                }
                else
                {
                    MELO_WARN_STREAM("Maxon '" << maxon_slave_ptr->getName()
                                                                         << "': " << maxon_slave_ptr->getReading().getDriveState());
                }

                // Constant update rate
                // std::this_thread::sleep_until(start_time + std::chrono::milliseconds(1));

            }
        }
        counter++;

        maxonEnabledAfterStartup = true;
    }
}

/*
** Handle the interrupt signal.
** This is the shutdown routine.
** Note: This logic is executed in a thread separated from the communication update!
 */
void signal_handler(int sig)
{
    /*
    ** Pre shutdown procedure.
    ** The devices execute procedures (e.g. state changes) that are necessary for a
    ** proper shutdown and that must be done with PDO communication.
    ** The communication update loop (i.e. PDO loop) continues to run!
    ** You might thus want to implement some logic that stages zero torque / velocity commands
    ** or simliar safety measures at this point using e.g. atomic variables and checking them
    ** in the communication update loop.
     */
    for(const auto & master: configurator->getMasters())
    {
        master->preShutdown();
    }

    // stop the PDO communication at the next update of the communication loop
    abrt = true;
    worker_thread->join();

    /*
    ** Completely halt the EtherCAT communication.
    ** No online communication is possible afterwards, including SDOs.
     */
    for(const auto & master: configurator->getMasters())
    {
        master->shutdown();
    }

    // Exit this executable
    std::cout << "Shutdown" << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    exit(0);
}

/*
** Program entry.
** Pass the path to the setup.yaml file as first command line argument.
 */
int main(int argc, char**argv)
{
    icecream::ic
        .prefix("\033[1;45m   IC   \033[0m ")
        .show_c_string(false);
    //icecream::ic.disable();

    // Set the abrt_ flag upon receiving an interrupt signal (e.g. Ctrl-c)
    std::signal(SIGINT, signal_handler);

    if(argc < 2)
    {
        std::cerr << "pass path to 'setup.yaml' as command line argument" << std::endl;
        return EXIT_FAILURE;
    }
    // a new EthercatDeviceConfigurator object (path to setup.yaml as constructor argument)
    configurator = std::make_shared<EthercatDeviceConfigurator>(argv[1]);

    /*
    ** Start all masters.
    ** There is exactly one bus per master which is also started.
    ** All online (i.e. SDO) configuration is done during this call.
    ** The EtherCAT interface is active afterwards, all drives are in Operational
    ** EtherCAT state and PDO communication may begin.
     */
    for(auto & master: configurator->getMasters())
    {
        if(!master->startup())
        {
            std::cerr << "Startup not successful." << std::endl;
            return EXIT_FAILURE;
        }
    }

    // Start the PDO loop in a new thread.
    worker_thread = std::make_unique<std::thread>(&worker);

    /*
    ** Wait for a few PDO cycles to pass.
     */
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for(auto & slave: configurator->getSlaves())
    {
        std::cout << " " << slave->getName() << ": " << slave->getAddress() << std::endl;
    }


    std::cout << "Startup finished" << std::endl;

    // nothing further to do in this thread.
    pause();
}
